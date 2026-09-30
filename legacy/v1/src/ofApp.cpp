#include "ofApp.h"

//--------------------------------------------------------------
// Teintes : échelle openFrameworks 0..255. ofColor::fromHsb() ne gère PAS le débordement
// (une teinte > ~306 renvoie du blanc), donc on ramène toujours la teinte dans [0, 255).
static float wrapHue(float h){
    h = fmodf(h, 255.0f);
    return h < 0.0f ? h + 255.0f : h;
}

static ofColor hsb(float h, float s, float b){
    return ofColor::fromHsb(wrapHue(h), s, b);
}

// Rapproche une teinte de sa cible par le plus court chemin sur le cercle chromatique
// (passer de 250 à 5 traverse 255/0, au lieu de refaire tout le tour).
static void approachHue(float & hue, float target, float rate){
    float diff = target - hue;
    if (diff > 127.5f) diff -= 255.0f;
    if (diff < -127.5f) diff += 255.0f;
    hue = wrapHue(hue + diff * ofClamp(rate, 0.0f, 1.0f));
}

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

    // Initialisation de la caméra (640x480 pour des performances optimales). Liste des
    // périphériques dispo une fois au démarrage -> choix dans le Dashboard (onglet Video/Webcam)
    // sans avoir à relancer l'appli si plusieurs caméras sont branchées.
    vidGrabber.setDesiredFrameRate(30);
    videoDevices = vidGrabber.listDevices();
    int defaultVideoId = 0;
    for (auto & d : videoDevices) if (d.bAvailable) { defaultVideoId = d.id; break; }
    applyVideoDevice(defaultVideoId);

    // Allocation des FBO à la résolution finale
    fbo.allocate(1920, 1080, GL_RGBA);
    lastFbo.allocate(1920, 1080, GL_RGBA);
    turingMaskSrc.allocate(1920, 1080, GL_RGBA);

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
    mapDarkTargetHue = mapDarkHue;
    genLightTargetHue = genLightHue;
    genMidTargetHue = genMidHue;

    // Entrée audio : capte le retour de la table de mix branché sur le jack de la carte son.
    // FFT réelle (kiss_fftr, déjà linkée) allouée sur une fenêtre d'analyse (audioFftSize) plus
    // grande que le buffer matériel de la carte son (audioBufferSize) : une FFT sur seulement
    // 512 échantillons ne résout les basses que sur ~3 bandes de fréquence, ce qui donnait une
    // estimation très bruitée (donc une réaction saccadée). 2048 échantillons -> ~12 bandes dans
    // le grave, beaucoup plus stable, sans changer la latence du buffer matériel.
    fftCfg = kiss_fftr_alloc(audioFftSize, 0, nullptr, nullptr);
    audioRingBuffer.assign(audioFftSize, 0.0f);
    fftSamples.assign(audioFftSize, 0.0f);
    fftSpectrum.assign(audioFftSize / 2 + 1, kiss_fft_cpx{0.0f, 0.0f});
    fftWindow.resize(audioFftSize);
    for (int i = 0; i < audioFftSize; i++) {
        fftWindow[i] = 0.5f * (1.0f - cosf(TWO_PI * i / (audioFftSize - 1))); // Hann
    }

    // Liste des entrées audio disponibles (table de mix en ligne, micro USB...) -> choix dans le
    // Dashboard (onglet Musique) au lieu de dépendre du device par défaut du système.
    audioInputDevices.clear();
    for (auto & d : soundStream.getDeviceList()) {
        if (d.inputChannels > 0) audioInputDevices.push_back(d);
    }
    int defaultAudioId = -1;
    for (auto & d : audioInputDevices) if (d.isDefaultInput) { defaultAudioId = d.deviceID; break; }
    if (defaultAudioId < 0 && !audioInputDevices.empty()) defaultAudioId = audioInputDevices.front().deviceID;
    applyAudioDevice(defaultAudioId);

    // Police du dashboard : la police bitmap par défaut d'OF (ofDrawBitmapString) est très
    // grossière, elle donnait cet air "mal fait". La TTF terminal-bold (déjà dans bin/data/fonts
    // mais jamais chargée) rend un vrai look terminal/pixel assumé, cohérent avec la direction
    // artistique du projet.
    // makeContours=true : on dessine les lettres comme des polygones vectoriels
    // (drawStringAsShapes) plutôt que via l'atlas texture de drawString(), qui s'est révélé
    // rendre des blocs pleins opaques au lieu des glyphes sur ce système.
    dashFont.load("fonts/terminal-bold.ttf", 24, true, true, true);
}

//--------------------------------------------------------------
void ofApp::applyVideoDevice(int deviceId){
    vidGrabber.close();
    vidGrabber.setDeviceID(deviceId);
    vidGrabber.setup(640, 480);
    selectedVideoDeviceId = deviceId;
}

//--------------------------------------------------------------
void ofApp::applyAudioDevice(int deviceId){
    soundStream.close();
    ofSoundStreamSettings soundSettings;
    for (auto & d : audioInputDevices) {
        if (d.deviceID == deviceId) { soundSettings.setInDevice(d); break; }
    }
    soundSettings.setInListener(this);
    soundSettings.sampleRate = 44100;
    soundSettings.numInputChannels = 2;
    soundSettings.numOutputChannels = 0;
    soundSettings.bufferSize = audioBufferSize;
    soundStream.setup(soundSettings);
    selectedAudioDeviceId = deviceId;
}

//--------------------------------------------------------------
void ofApp::update(){
    if ((int)targetFPS != appliedFPS) {
        appliedFPS = (int)targetFPS;
        ofSetFrameRate(appliedFPS);
    }
    vidGrabber.update();

    float dt = ofGetLastFrameTime();
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    // --- Analyse audio : enveloppes lissées avec de vraies constantes de temps (secondes),
    // calculées AVANT les dérives de couleur ci-dessous pour pouvoir moduler leur vitesse avec
    // la musique. Fait en premier aussi parce que bassKick sert au zoom/flash dans draw().
    if (audioReactive) {
        float bass, mid, treble;
        {
            std::lock_guard<std::mutex> lock(audioMutex);
            bass = bassRaw; mid = midRaw; treble = trebleRaw;
        }

        // Auto-normalisation : chaque bande suit son propre maximum récent (qui redescend
        // lentement), pour que la réaction garde la même amplitude visuelle du début à la fin
        // d'une soirée, même si les morceaux n'ont pas tous le même niveau -> plus besoin de
        // remonter "Sensibilite" à chaque changement de morceau.
        float peakDecay = expf(-dt / 6.0f);
        bassPeak = std::max(bass, bassPeak * peakDecay);
        midPeak = std::max(mid, midPeak * peakDecay);
        treblePeak = std::max(treble, treblePeak * peakDecay);
        float bassN = bassPeak > 0.0001f ? ofClamp(bass / bassPeak, 0.0f, 1.0f) : 0.0f;
        float midN = midPeak > 0.0001f ? ofClamp(mid / midPeak, 0.0f, 1.0f) : 0.0f;
        float trebN = treblePeak > 0.0001f ? ofClamp(treble / treblePeak, 0.0f, 1.0f) : 0.0f;

        // Enveloppes "niveau" (attaque rapide, retombée douce) : des constantes de temps en
        // secondes, pas un pourcentage fixe par frame -> le rendu reste identique à 20 FPS ou à
        // 60 FPS (avant, la vitesse de lissage dépendait du framerate, une des causes des à-coups).
        auto smoothTowards = [dt](float & value, float target, float attackTau, float releaseTau){
            float tau = target > value ? attackTau : releaseTau;
            value += (target - value) * (1.0f - expf(-dt / tau));
        };
        smoothTowards(bassSmooth, bassN, 0.05f, 0.4f);
        smoothTowards(midSmooth, midN, 0.08f, 0.5f);
        smoothTowards(trebleSmooth, trebN, 0.06f, 0.45f);

        // Pulsation "kick" séparée : attaque quasi instantanée, retombée exponentielle nette ->
        // un vrai coup qui reflue pour le zoom/flash, distinct du niveau lissé ci-dessus qui sert
        // à moduler des choses continues (vitesse de dérive, intensité de la nébuleuse...).
        smoothTowards(bassKick, bassN, 0.015f, 0.25f);

        // --- Détection de temps (onset) : un temps est détecté quand les basses dépassent
        // nettement leur moyenne locale récente (détection par flux d'énergie). Une période
        // réfractaire (180ms) empêche un seul coup de grosse caisse de déclencher deux fois.
        float now = ofGetElapsedTimef();
        bool onset = (bassN > bassLocalAvg * 1.35f + 0.05f) && (now - lastOnsetTime > 0.18f);
        if (onset) {
            float interval = now - lastOnsetTime;
            // On n'accepte l'écart pour l'estimation de tempo que s'il correspond à un tempo
            // plausible (60-200 BPM) : ignore les silences (intro/break) ou les doubles-croches
            // isolées qui fausseraient l'estimation.
            if (interval > 0.3f && interval < 1.0f) {
                float instantBpm = 60.0f / interval;
                bpmEstimate = (bpmEstimate <= 1.0f) ? instantBpm : bpmEstimate + (instantBpm - bpmEstimate) * 0.25f;
            }
            lastOnsetTime = now;
            beatPulse = 1.0f;
        } else {
            beatPulse *= expf(-dt / 0.12f);
        }
        // Moyenne locale de suivi (plus lente que bassSmooth) : la référence par rapport à
        // laquelle un temps est jugé "nettement plus fort que ce qui vient de se passer".
        bassLocalAvg += (bassN - bassLocalAvg) * (1.0f - expf(-dt / 0.6f));
    } else {
        // Retour au calme si on désactive la réactivité en pleine pulsation
        bassKick *= expf(-dt / 0.3f);
        beatPulse *= expf(-dt / 0.3f);
    }

    // Dérive Video/Webcam : PAS une rotation continue autour du cercle chromatique.
    // On saute vers une nouvelle teinte cible tirée au hasard, on transitionne en douceur
    // vers elle, on attend, puis on retire une nouvelle cible -> "passer d'une couleur à
    // une autre" plutôt que "tourner en rond". Les deux couleurs (claire ET sombre) dérivent
    // chacune sur leur propre timer ; en musique, la vitesse de transition suit l'énergie du
    // morceau (plus vif sur les passages chargés, plus calme sur les breaks).
    float driftMult = audioReactive ? 1.0f + midSmooth * audioSpeedAmount * 2.0f : 1.0f;

    if (mapAutoMode) {
        float rate = mapColorSpeed * driftMult * dt;
        mapLightHueTimer -= dt;
        if (mapLightHueTimer <= 0.0f) {
            mapLightTargetHue = ofRandom(0.0f, 255.0f);
            mapLightHueTimer = ofRandom(3.0f, 7.0f);
        }
        approachHue(mapLightHue, mapLightTargetHue, rate);

        // La couleur sombre garde une cible autour de la complémentaire de la claire (contraste
        // toujours assuré) mais dérive vers elle indépendamment, sur son propre timer -> les deux
        // couleurs bougent vraiment, elles ne se contentent pas de se suivre l'une l'autre.
        mapDarkHueTimer -= dt;
        if (mapDarkHueTimer <= 0.0f) {
            mapDarkTargetHue = wrapHue(mapLightHue + 128.0f + ofRandom(-50.0f, 50.0f));
            mapDarkHueTimer = ofRandom(3.0f, 7.0f);
        }
        approachHue(mapDarkHue, mapDarkTargetHue, rate);
    }

    // Dérive Mode Génératif : même principe, sur les deux couleurs qui comptent le plus
    // visuellement (claire + accent) ; la couleur creux/noir reste le ton stable de fond.
    if (autoMode) {
        float rate = colorSpeed * driftMult * dt;
        genLightHueTimer -= dt;
        if (genLightHueTimer <= 0.0f) {
            genLightTargetHue = ofRandom(0.0f, 255.0f);
            genLightHueTimer = ofRandom(3.0f, 7.0f);
        }
        approachHue(genLightHue, genLightTargetHue, rate);

        genMidHueTimer -= dt;
        if (genMidHueTimer <= 0.0f) {
            genMidTargetHue = wrapHue(genLightHue + ofRandom(60.0f, 180.0f));
            genMidHueTimer = ofRandom(3.0f, 7.0f);
        }
        approachHue(genMidHue, genMidTargetHue, rate);
    }

    // Simulation de Turing : seulement si la couche est active (coûteux pour rien sinon)
    if (showTuringTab) {
        if (turingAutoDrift) {
            turingDriftPhase += dt * turingDriftSpeed;
        }
        stepTuringSimulation();
    }
}

//--------------------------------------------------------------
void ofApp::audioIn(ofSoundBuffer & buffer){
    int nCh = buffer.getNumChannels();
    int n = buffer.getNumFrames();
    if (n != audioBufferSize || nCh <= 0 || !fftCfg || (int)audioRingBuffer.size() != audioFftSize) return;

    // Downmix vers mono + gain, empilé dans un anneau qui garde les derniers audioFftSize
    // échantillons (plus grand que le buffer matériel) : la FFT ci-dessous analyse une fenêtre
    // plus longue -> bien meilleure résolution en fréquence, surtout critique dans le grave.
    for (int i = 0; i < n; i++) {
        float s = 0;
        for (int c = 0; c < nCh; c++) s += buffer.getSample(i, c);
        audioRingBuffer[audioRingWritePos] = (s / nCh) * audioGain;
        audioRingWritePos = (audioRingWritePos + 1) % audioFftSize;
    }

    // Fenêtrage de Hann appliqué sur toute la fenêtre d'analyse (limite les fuites spectrales)
    for (int i = 0; i < audioFftSize; i++) {
        int idx = (audioRingWritePos + i) % audioFftSize;
        fftSamples[i] = audioRingBuffer[idx] * fftWindow[i];
    }

    auto & spectrum = fftSpectrum;
    kiss_fftr(fftCfg, fftSamples.data(), spectrum.data());

    float sampleRate = buffer.getSampleRate() > 0 ? buffer.getSampleRate() : 44100.0f;
    float binHz = sampleRate / audioFftSize;
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

    // Réactivité audio : les basses font flasher/zoomer la vidéo (s'applique aux deux onglets).
    // Le flash utilise bassSmooth (niveau lissé, continu) pour une lueur qui respire avec le
    // morceau ; le zoom utilise bassKick (pulsation à retombée rapide) pour un vrai coup sur
    // chaque temps, sans à-coups au frame près.
    float currentZoom = zoomLevel;
    int currentCamOpacity = (int)camOpacity;
    if (audioReactive) {
        currentCamOpacity = ofClamp(currentCamOpacity + (int)(bassSmooth * audioColorAmount * 400.0f), 0, 255);
        currentZoom += (audioBeatSync ? beatPulse : bassKick) * audioZoomAmount * 0.15f;
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

        ofColor lc = hsb(mapLightHue, mapLightSat, mapLightBright);
        ofColor dc = hsb(mapDarkHue, mapDarkSat, mapDarkBright);

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

    // 3bis. ONGLET TURING (réaction-diffusion) : même principe, jamais réinjecté dans le feedback.
    // On capture d'abord l'image composée (webcam + génératif) : c'est elle qui sert de masque
    // de luminosité si l'utilisateur choisit "zones claires" / "zones sombres".
    if (showTuringTab) {
        turingMaskSrc.begin();
        ofClear(0, 0, 0, 255);
        ofSetColor(255);
        fbo.draw(0, 0);
        turingMaskSrc.end();

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
    ofColor c1 = hsb(genLightHue, genLightSat, genLightBright);
    ofColor c2 = hsb(genMidHue, genMidSat, genMidBright);
    ofColor c3 = hsb(genDarkHue, genDarkSat, genDarkBright);

    ofEnableBlendMode(OF_BLENDMODE_ADD);
    genShader.begin();
    genShader.setUniform1f("u_time", ofGetElapsedTimef() * evolutionSpeed);
    genShader.setUniform1f("u_aspect", fbo.getWidth() / fbo.getHeight());
    genShader.setUniform2f("u_resolution", fbo.getWidth(), fbo.getHeight());
    genShader.setUniform1f("u_nebulaScale", nebulaScale);
    genShader.setUniform1f("u_nebulaWarp", nebulaWarp);
    genShader.setUniform1f("u_nebulaContrast", nebulaContrast);
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
    // Dérive lente de Feed/Kill autour du réglage choisi (deux fréquences différentes pour ne
    // pas juste faire l'aller-retour sur une ligne droite) : le motif change progressivement de
    // "territoire" Gray-Scott au lieu de rester bloqué sur le même point de fonctionnement.
    float f = turingFeed;
    float k = turingKill;
    if (turingAutoDrift) {
        f += sinf(turingDriftPhase) * turingDriftAmount;
        k += cosf(turingDriftPhase * 0.63f) * turingDriftAmount * 0.8f;
    }

    int steps = std::max(1, (int)turingSteps);
    for (int i = 0; i < steps; i++) {
        ofFbo & src = rdPingPong ? rdBufferB : rdBufferA;
        ofFbo & dst = rdPingPong ? rdBufferA : rdBufferB;

        dst.begin();
        rdShader.begin();
        rdShader.setUniform1f("feed", f);
        rdShader.setUniform1f("kill", k);
        rdShader.setUniform1f("du", 1.0f);
        rdShader.setUniform1f("dv", turingDiffV);
        rdShader.setUniform1f("u_time", ofGetElapsedTimef() + i * 0.013f);
        ofSetColor(255);
        src.draw(0, 0);
        rdShader.end();
        dst.end();

        rdPingPong = !rdPingPong;
    }
}

//--------------------------------------------------------------
void ofApp::drawTuringVisuals(){
    ofColor c1 = hsb(turingLightHue, turingLightSat, turingLightBright);
    ofColor c2 = hsb(turingMidHue, turingMidSat, turingMidBright);
    ofColor c3 = hsb(turingDarkHue, turingDarkSat, turingDarkBright);

    ofFbo & result = rdPingPong ? rdBufferB : rdBufferA;

    ofEnableBlendMode(OF_BLENDMODE_ADD);
    turingDisplayShader.begin();
    turingDisplayShader.setUniform3f("u_colorLight", c1.r/255.0f, c1.g/255.0f, c1.b/255.0f);
    turingDisplayShader.setUniform3f("u_colorMid", c2.r/255.0f, c2.g/255.0f, c2.b/255.0f);
    turingDisplayShader.setUniform3f("u_colorDark", c3.r/255.0f, c3.g/255.0f, c3.b/255.0f);
    turingDisplayShader.setUniformTexture("u_maskTex", turingMaskSrc.getTexture(), 1);
    turingDisplayShader.setUniform2f("u_maskSize", turingMaskSrc.getWidth(), turingMaskSrc.getHeight());
    turingDisplayShader.setUniform2f("u_rdSize", (float)rdWidth, (float)rdHeight);
    turingDisplayShader.setUniform1i("u_maskMode", turingMaskMode);
    turingDisplayShader.setUniform1f("u_maskThreshold", turingMaskThreshold);
    ofSetColor(255);
    result.draw(0, 0, fbo.getWidth(), fbo.getHeight());
    turingDisplayShader.end();
    ofDisableBlendMode();
}

//======================================================================
// DASHBOARD CUSTOM : mini kit d'UI (dessin + hit-test), layout 100% dynamique
//======================================================================

//--------------------------------------------------------------
float ofApp::collageAngle(float x, float y){
    // Déterministe (même x,y -> même angle à chaque frame, pas de tremblement) mais irrégulier
    // d'un widget à l'autre : donne l'impression de morceaux de papier découpés puis recollés.
    // Amplitude volontairement discrète (comme un vrai collage, tout n'est pas de travers).
    float s = sinf(x * 12.9898f + y * 78.233f) * 43758.5453f;
    s -= floorf(s); // fract -> 0..1
    return (s - 0.5f) * 3.0f; // -1.5° .. 1.5°
}

//--------------------------------------------------------------
// Petit bruit déterministe (même entrée -> même sortie, stable d'une frame à l'autre) utilisé
// pour jitterer les bords des chips ("papier déchiré") et l'ombre portée.
static float collageNoise(float x, float y){
    float s = sinf(x * 12.9898f + y * 78.233f) * 43758.5453f;
    return s - floorf(s); // 0..1
}

//--------------------------------------------------------------
void ofApp::drawCollageChip(ofRectangle rect, ofColor fillColor, ofColor borderColor, bool filled){
    float angle = collageAngle(rect.x, rect.y);
    float w = rect.width, h = rect.height;

    // Polygone à bord irrégulier (8 points : 4 coins + 1 point jitteré au milieu de chaque
    // bord) plutôt qu'un rectangle parfait -> lit vraiment comme un bout de papier découpé,
    // pas comme "une case penchée".
    float amt = ofClamp(std::min(w, h) * 0.035f, 1.5f, 4.5f);
    auto jit = [&](float sx, float sy){ return (collageNoise(sx, sy) - 0.5f) * 2.0f * amt; };

    std::vector<ofPoint> pts;
    pts.push_back(ofPoint(jit(rect.x, rect.y), jit(rect.x + 1, rect.y + 1)));
    pts.push_back(ofPoint(w / 2, jit(rect.x + w / 2, rect.y) - amt * 0.6f));
    pts.push_back(ofPoint(w + jit(rect.x + w, rect.y + 2), jit(rect.x + w + 1, rect.y + 3)));
    pts.push_back(ofPoint(w + jit(rect.x + w, rect.y + h / 2) + amt * 0.6f, h / 2));
    pts.push_back(ofPoint(w + jit(rect.x + w + 4, rect.y + h), h + jit(rect.x + w + 5, rect.y + h + 1)));
    pts.push_back(ofPoint(w / 2, h + jit(rect.x + w / 2, rect.y + h + 2) + amt * 0.6f));
    pts.push_back(ofPoint(jit(rect.x, rect.y + h + 3), h + jit(rect.x + 1, rect.y + h + 4)));
    pts.push_back(ofPoint(jit(rect.x - 4, rect.y + h / 2) - amt * 0.6f, h / 2));

    auto drawPoly = [&](){
        ofBeginShape();
        for (auto & p : pts) ofVertex(p.x, p.y);
        ofEndShape(true);
    };

    ofPushMatrix();
    ofTranslate(rect.x + w / 2, rect.y + h / 2);
    ofRotateDeg(angle);
    ofTranslate(-w / 2, -h / 2);

    // Ombre portée : silhouette du même polygone, légèrement décalée -> vraie impression de
    // papier posé sur la page, plus sobre qu'un aplat crème répété partout.
    ofEnableAlphaBlending();
    ofPushMatrix();
    ofTranslate(4, 5);
    ofSetColor(0, 0, 0, 110);
    drawPoly();
    ofPopMatrix();
    ofDisableAlphaBlending();

    ofSetColor(filled ? fillColor : ofColor(16, 16, 16));
    drawPoly();

    ofNoFill();
    ofSetColor(borderColor);
    ofSetLineWidth(2.5f);
    drawPoly();
    ofFill();

    ofPopMatrix();
}

//--------------------------------------------------------------
void ofApp::drawStarShape(float x, float y, float outerR, float innerR, int points, float rotationDeg, ofColor color){
    ofPushMatrix();
    ofTranslate(x, y);
    ofRotateDeg(rotationDeg);
    ofSetColor(color);
    ofBeginShape();
    for (int i = 0; i < points * 2; i++) {
        float r = (i % 2 == 0) ? outerR : innerR;
        float a = i * PI / points;
        ofVertex(cosf(a) * r, sinf(a) * r);
    }
    ofEndShape(true);
    ofPopMatrix();
}

//--------------------------------------------------------------
void ofApp::uiText(const std::string & text, float x, float y, ofColor color, float scale){
    // dashFont est chargée à 24px ; on ramène à l'échelle des anciens réglages (calibrés pour
    // la police bitmap ~8px) pour ne pas avoir à retoucher tous les appels existants.
    float f = (8.0f / 24.0f) * scale;
    ofSetColor(color);
    ofPushMatrix();
    ofTranslate(x, y);
    ofScale(f, f);
    dashFont.drawStringAsShapes(text, 0, 0);
    ofPopMatrix();
}

//--------------------------------------------------------------
float ofApp::uiTextWidth(const std::string & text, float scale){
    return dashFont.stringWidth(text) * (8.0f / 24.0f) * scale;
}

//--------------------------------------------------------------
bool ofApp::uiCollapsibleHeader(const std::string & text, float x, float y, float w, ofColor accent, bool & expanded){
    ofRectangle rect(x, y - 6, w, 32);
    drawCollageChip(rect, ofColor(20, 20, 20), accent, false);

    std::string arrow = expanded ? "[-] " : "[+] ";
    uiText(arrow + text, x + 10, y + 14, accent, 1.5f);
    // Petite étoile punk en bout de bandeau, dans la couleur d'accent de la section
    drawStarShape(x + w - 20, y + 10, 9, 4, 5, collageAngle(rect.x, rect.y) * 20.0f, accent);

    liveToggles.push_back({ &expanded, rect });
    return expanded;
}

//--------------------------------------------------------------
void ofApp::uiToggleCard(const std::string & label, ofRectangle rect, bool & value, ofColor accent){
    drawCollageChip(rect, accent, accent, value);
    float scale = 1.6f;
    float tw = uiTextWidth(label, scale);
    ofColor textColor = value ? ofColor(0, 0, 0) : accent;
    uiText(label, rect.x + (rect.width - tw) / 2, rect.y + rect.height / 2 + 6, textColor, scale);
    liveToggles.push_back({ &value, rect });
}

//--------------------------------------------------------------
void ofApp::uiTabButton(const std::string & label, ofRectangle rect, bool active, ofColor accent, std::function<void()> onClick){
    drawCollageChip(rect, accent, accent, active);
    ofColor textColor = active ? ofColor(0, 0, 0) : accent;
    float scale = 1.4f;
    float tw = uiTextWidth(label, scale);
    uiText(label, rect.x + (rect.width - tw) / 2, rect.y + rect.height / 2 + 5, textColor, scale);
    liveButtons.push_back({ rect, onClick });
}

//--------------------------------------------------------------
void ofApp::uiButton(const std::string & label, ofRectangle rect, ofColor accent, std::function<void()> onClick){
    drawCollageChip(rect, accent, accent, true);
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

    // Piste fine "pilule" (arrondie) + poignée en losange, façon lecteur moderne, plutôt que
    // l'ancienne barre pleine hauteur : plus lisible, plus compact, et une vraie zone de préhension
    // (hitRect) plus grande que la piste visible pour rester facile à saisir à la souris.
    float trackY = y + 26;
    float trackH = 6;
    float hitH = 26;
    ofRectangle hitRect(x, trackY - (hitH - trackH) / 2.0f, w, hitH);

    float t = ofClamp((value - minV) / (maxV - minV), 0.0f, 1.0f);

    ofSetColor(30, 30, 30);
    ofDrawRectRounded(x, trackY, w, trackH, trackH / 2.0f);
    if (t > 0.001f) {
        ofSetColor(60, 255, 130, 130);
        ofDrawRectRounded(x, trackY, w * t, trackH, trackH / 2.0f);
    }

    drawSliderThumb(x + w * t, trackY + trackH / 2.0f, 9.0f, ofColor(235, 255, 30));

    liveSliders.push_back({ &value, minV, maxV, isInt, hitRect });
    return trackY + trackH + 24;
}

//--------------------------------------------------------------
void ofApp::drawSliderThumb(float x, float y, float size, ofColor color){
    // Losange plutôt qu'un rond générique : reprend le motif des étoiles/accents du reste du
    // dashboard au lieu d'un curseur de lecteur audio générique.
    ofPushMatrix();
    ofTranslate(x, y);
    ofRotateDeg(45.0f);

    ofSetColor(0, 0, 0, 90);
    ofDrawRectangle(-size / 2.0f + 1.5f, -size / 2.0f + 2.0f, size, size);

    ofSetColor(255);
    ofDrawRectangle(-size / 2.0f - 2.0f, -size / 2.0f - 2.0f, size + 4.0f, size + 4.0f);

    ofSetColor(color);
    ofDrawRectangle(-size / 2.0f, -size / 2.0f, size, size);

    ofPopMatrix();
}

//--------------------------------------------------------------
void ofApp::generateColorWheelImage(int size){
    ofPixels pix;
    pix.allocate(size, size, OF_PIXELS_RGBA);
    float radius = size / 2.0f;
    ofPoint center(radius, radius);

    for (int yy = 0; yy < size; yy++) {
        for (int xx = 0; xx < size; xx++) {
            float dx = xx - center.x;
            float dy = yy - center.y;
            float dist = sqrt(dx * dx + dy * dy);
            ofColor c;
            if (dist <= radius) {
                float angle = atan2(dy, dx);
                float hueDeg = angle * 180.0f / PI;
                if (hueDeg < 0) hueDeg += 360;
                float hue = hueDeg / 360.0f * 255.0f;
                float sat = ofClamp(dist / radius, 0.0f, 1.0f) * 255.0f;
                c = ofColor::fromHsb(hue, sat, 255);
                c.a = 255;
            } else {
                c = ofColor(0, 0, 0, 0);
            }
            pix.setColor(xx, yy, c);
        }
    }
    colorWheelImg.setFromPixels(pix);
}

//--------------------------------------------------------------
float ofApp::uiColorEditor(const std::string & label, float x, float y, float w, float & h, float & s, float & b){
    if (!colorWheelReady) {
        generateColorWheelImage(160);
        colorWheelReady = true;
    }

    uiText(label, x, y + 12, ofColor(255, 255, 255), 1.4f);

    ofColor preview = hsb(h, s, b);
    ofRectangle swatch(x + w - 56, y - 6, 46, 28);
    drawCollageChip(swatch, preview, ofColor(255), true);

    y += 30;

    // Compact : suffisant pour choisir une teinte au pouce/à la souris sans occuper l'écran
    float diameter = 104;
    ofEnableAlphaBlending();
    ofSetColor(255);
    colorWheelImg.draw(x, y, diameter, diameter);
    ofDisableAlphaBlending();

    ofPoint center(x + diameter / 2, y + diameter / 2);
    float radius = diameter / 2;

    float angleRad = (h / 255.0f) * TWO_PI;
    float rr = (s / 255.0f) * radius;
    float ix = center.x + cos(angleRad) * rr;
    float iy = center.y + sin(angleRad) * rr;
    ofNoFill();
    ofSetColor(255);
    ofSetLineWidth(2);
    ofDrawCircle(ix, iy, 6);
    ofFill();

    liveWheels.push_back({ &h, &s, center, radius });

    y += diameter + 10;
    y = uiSliderRow("  Luminosite", x, y, w, b, 0.0f, 255.0f);
    return y + 6;
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
void ofApp::applyWheelDrag(int index, float mouseX, float mouseY){
    if (index < 0 || index >= (int)liveWheels.size()) return;
    auto & w = liveWheels[index];
    float dx = mouseX - w.center.x;
    float dy = mouseY - w.center.y;
    float dist = sqrt(dx * dx + dy * dy);

    float angle = atan2(dy, dx);
    float hueDeg = angle * 180.0f / PI;
    if (hueDeg < 0) hueDeg += 360;
    *(w.hue) = hueDeg / 360.0f * 255.0f;
    *(w.sat) = ofClamp(dist / w.radius, 0.0f, 1.0f) * 255.0f;
}

//--------------------------------------------------------------
void ofApp::drawDashboard(ofEventArgs &){
    ofBackground(8, 8, 8);
    liveSliders.clear();
    liveToggles.clear();
    liveButtons.clear();
    liveWheels.clear();

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

    // Titre façon affiche : double frappe légèrement décalée (ombre acide) derrière le vert
    // phosphorescent habituel, plus une étoile en accroche -> un vrai en-tête, pas juste un label.
    uiText("BNS VISUALGEN", x + 3, y + 11, ofColor(235, 20, 120, 200), 2.2f);
    uiText("BNS VISUALGEN", x, y + 8, ofColor(60, 255, 130), 2.2f);
    drawStarShape(x + uiTextWidth("BNS VISUALGEN", 2.2f) + 22, y + 2, 11, 5, 5, -12.0f, ofColor(255, 210, 30));
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

        // Choix de l'entrée audio (table de mix en ligne, micro USB...) : chaque périphérique
        // détecté au démarrage est une carte cliquable, la carte active est mise en surbrillance.
        uiText("  ENTREE AUDIO", x, y + 12, ofColor(255, 255, 255), 1.4f);
        y += 30;
        if (audioInputDevices.empty()) {
            uiText("  Aucune entree audio detectee", x, y + 10, ofColor(200, 80, 80), 1.1f);
            y += 32;
        } else {
            for (auto & dev : audioInputDevices) {
                ofRectangle r(x, y, w, 42);
                bool active = (dev.deviceID == selectedAudioDeviceId);
                drawCollageChip(r, ofColor(60, 255, 130), ofColor(60, 255, 130), active);
                float scale = 1.05f;
                uiText(dev.name, r.x + 16, r.y + r.height / 2 + 4, active ? ofColor(0, 0, 0) : ofColor(60, 255, 130), scale);
                int devId = dev.deviceID;
                liveButtons.push_back({ r, [this, devId](){ applyAudioDevice(devId); } });
                y += 42 + 10;
            }
        }
        y += 14;

        y = uiSliderRow("Sensibilite Micro/Ligne", x, y, w, audioGain, 0.5f, 10.0f);
        y = uiSliderRow("Basses -> Flash", x, y, w, audioColorAmount, 0.0f, 3.0f);
        y = uiSliderRow("Basses -> Zoom Kick", x, y, w, audioZoomAmount, 0.0f, 3.0f);
        y = uiSliderRow("Aigus -> Vitesse Derive", x, y, w, audioSpeedAmount, 0.0f, 3.0f);

        y += 6;
        uiToggleCard(audioBeatSync ? "ZOOM SUR LES TEMPS : ACTIF" : "ZOOM SUR LES TEMPS : COUPE", ofRectangle(x, y, w, 54), audioBeatSync, ofColor(235, 20, 120));
        y += 54 + 12;
        uiText("  (Actif = coup net sur chaque temps detecte ; Coupe = suit le niveau en continu, plus organique)", x, y, ofColor(140, 140, 140), 1.0f);
        y += 30;

        // Lecture du tempo détecté : purement informatif, permet de vérifier que la détection
        // "tient" le morceau avant de compter sur elle pendant un set.
        std::string bpmLabel = audioReactive && bpmEstimate > 1.0f
            ? "TEMPO DETECTE : " + ofToString((int)std::round(bpmEstimate)) + " BPM"
            : "TEMPO DETECTE : --";
        uiText("  " + bpmLabel, x, y + 12, ofColor(60, 255, 130), 1.4f);
        y += 34;
    } else if (activeTab == DashTab::VIDEO) {
        // Choix de la caméra : une carte cliquable par périphérique détecté au démarrage.
        uiText("  CAMERA", x, y + 12, ofColor(255, 255, 255), 1.4f);
        y += 30;
        if (videoDevices.empty()) {
            uiText("  Aucune camera detectee", x, y + 10, ofColor(200, 80, 80), 1.1f);
            y += 32;
        } else {
            for (auto & dev : videoDevices) {
                if (!dev.bAvailable) continue;
                ofRectangle r(x, y, w, 42);
                bool active = (dev.id == selectedVideoDeviceId);
                drawCollageChip(r, ofColor(255, 0, 150), ofColor(255, 0, 150), active);
                float scale = 1.05f;
                uiText(dev.deviceName, r.x + 16, r.y + r.height / 2 + 4, active ? ofColor(0, 0, 0) : ofColor(255, 0, 150), scale);
                int devId = dev.id;
                liveButtons.push_back({ r, [this, devId](){ applyVideoDevice(devId); } });
                y += 42 + 10;
            }
        }
        y += 14;

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
        y = uiSliderRow("Vitesse Derive (lent)", x, y, w, evolutionSpeed, 0.0f, 1.0f);
        y = uiSliderRow("Echelle Nebuleuse", x, y, w, nebulaScale, 0.3f, 4.0f);
        y = uiSliderRow("Intensite Deformation", x, y, w, nebulaWarp, 0.0f, 8.0f);
        y = uiSliderRow("Contraste", x, y, w, nebulaContrast, 0.3f, 4.0f);
    } else {
        // TURING : réaction-diffusion de Gray-Scott -> motifs organiques (taches, corail, rayures)
        y = uiColorEditor("COULEUR CLAIRE", x, y, w, turingLightHue, turingLightSat, turingLightBright);
        y = uiColorEditor("COULEUR ACCENT", x, y, w, turingMidHue, turingMidSat, turingMidBright);
        y = uiColorEditor("COULEUR CREUX/NOIR", x, y, w, turingDarkHue, turingDarkSat, turingDarkBright);
        uiButton("REENSEMENCER", ofRectangle(x, y, w, 50), ofColor(0, 220, 170), [this](){ turingReseedRequested = true; });
        y += 50 + 24;

        // Zone d'application : plein écran, ou uniquement sur les zones claires/sombres de l'image
        // en dessous (webcam + génératif), pour donner du relief au motif au lieu de tout recouvrir.
        uiText("  ZONE D'APPLICATION", x, y + 12, ofColor(255, 255, 255), 1.4f);
        y += 30;
        {
            // Une couleur punk différente par option : plus facile à repérer d'un coup d'oeil
            // qu'un accent monochrome, et ça colle à l'esprit collage du reste du dashboard.
            float mw = (w - 2 * 12) / 3.0f;
            auto maskCard = [&](const std::string & label, int mode, float cardX, ofColor accent){
                ofRectangle r(cardX, y, mw, 48);
                bool active = (turingMaskMode == mode);
                drawCollageChip(r, accent, accent, active);
                float scale = 1.05f;
                float tw = uiTextWidth(label, scale);
                uiText(label, r.x + (r.width - tw) / 2, r.y + r.height / 2 + 4, active ? ofColor(0, 0, 0) : accent, scale);
                liveButtons.push_back({ r, [this, mode](){ turingMaskMode = mode; } });
            };
            maskCard("PLEIN ECRAN", 0, x, ofColor(0, 220, 170));
            maskCard("ZONES CLAIRES", 1, x + mw + 12, ofColor(255, 190, 20));
            maskCard("ZONES SOMBRES", 2, x + 2 * (mw + 12), ofColor(180, 80, 255));
            y += 48 + 18;
        }
        if (turingMaskMode != 0) {
            y = uiSliderRow("Seuil de luminosite", x, y, w, turingMaskThreshold, 0.0f, 1.0f);
        }

        y = uiSliderRow("Feed (apport)", x, y, w, turingFeed, 0.01f, 0.09f);
        y = uiSliderRow("Kill (disparition)", x, y, w, turingKill, 0.03f, 0.08f);
        y = uiSliderRow("Diffusion V", x, y, w, turingDiffV, 0.2f, 1.0f);
        y = uiSliderRow("Vitesse (pas/frame)", x, y, w, turingSteps, 1.0f, 20.0f, true);

        y += 6;
        // Beaucoup de couples Feed/Kill finissent par saturer l'espace et arrêter toute activité :
        // cette dérive balade légèrement le point de fonctionnement pour que le motif continue de
        // se réorganiser au lieu de se figer au bout de quelques secondes.
        uiToggleCard(turingAutoDrift ? "DERIVE AUTO : ACTIVE" : "DERIVE AUTO : COUPEE", ofRectangle(x, y, w, 54), turingAutoDrift, ofColor(0, 220, 170));
        y += 54 + 24;
        if (turingAutoDrift) {
            y = uiSliderRow("Vitesse Derive", x, y, w, turingDriftSpeed, 0.01f, 0.3f);
            y = uiSliderRow("Amplitude Derive", x, y, w, turingDriftAmount, 0.0f, 0.03f);
        }
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
// Table unique des paramètres sauvegardés dans les presets : les clés JSON sont celles des
// anciens fichiers (compatibilité des presets existants). Ajouter un paramètre = une ligne ici.
std::vector<std::pair<const char *, float *>> ofApp::presetFloats(){
    return {
        {"audioGain", &audioGain}, {"audioColorAmount", &audioColorAmount},
        {"audioZoomAmount", &audioZoomAmount}, {"audioSpeedAmount", &audioSpeedAmount},
        {"mapLightHue", &mapLightHue}, {"mapLightSat", &mapLightSat}, {"mapLightBright", &mapLightBright},
        {"mapDarkHue", &mapDarkHue}, {"mapDarkSat", &mapDarkSat}, {"mapDarkBright", &mapDarkBright},
        {"mapColorSpeed", &mapColorSpeed},
        {"zoomLevel", &zoomLevel}, {"rotationAngle", &rotationAngle}, {"feedbackOpacity", &feedbackOpacity},
        {"camOpacity", &camOpacity}, {"threshold", &threshold}, {"targetFPS", &targetFPS},
        {"genLightHue", &genLightHue}, {"genLightSat", &genLightSat}, {"genLightBright", &genLightBright},
        {"genMidHue", &genMidHue}, {"genMidSat", &genMidSat}, {"genMidBright", &genMidBright},
        {"genDarkHue", &genDarkHue}, {"genDarkSat", &genDarkSat}, {"genDarkBright", &genDarkBright},
        {"colorSpeed", &colorSpeed},
        {"evolutionSpeed", &evolutionSpeed}, {"nebulaScale", &nebulaScale}, {"nebulaWarp", &nebulaWarp},
        {"nebulaContrast", &nebulaContrast},
        {"turingLightHue", &turingLightHue}, {"turingLightSat", &turingLightSat}, {"turingLightBright", &turingLightBright},
        {"turingMidHue", &turingMidHue}, {"turingMidSat", &turingMidSat}, {"turingMidBright", &turingMidBright},
        {"turingDarkHue", &turingDarkHue}, {"turingDarkSat", &turingDarkSat}, {"turingDarkBright", &turingDarkBright},
        {"turingFeed", &turingFeed}, {"turingKill", &turingKill}, {"turingDiffV", &turingDiffV},
        {"turingSteps", &turingSteps}, {"turingMaskThreshold", &turingMaskThreshold},
        {"turingDriftSpeed", &turingDriftSpeed}, {"turingDriftAmount", &turingDriftAmount},
    };
}

std::vector<std::pair<const char *, bool *>> ofApp::presetBools(){
    return {
        {"showVideoTab", &showVideoTab}, {"showGenerativeTab", &showGenerativeTab}, {"showTuringTab", &showTuringTab},
        {"audioReactive", &audioReactive}, {"audioBeatSync", &audioBeatSync},
        {"mapAutoMode", &mapAutoMode}, {"autoMode", &autoMode}, {"turingAutoDrift", &turingAutoDrift},
    };
}

//--------------------------------------------------------------
void ofApp::savePreset(int slot){
    ofJson j;
    for (auto & [key, ptr] : presetFloats()) j[key] = *ptr;
    for (auto & [key, ptr] : presetBools()) j[key] = *ptr;
    j["turingMaskMode"] = turingMaskMode;

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
    if (!j.is_object()) {
        ofLogError("BNS-VisualGen") << "Preset " << slot << " : fichier illisible (" << path << ").";
        return;
    }

    // j.value(clé, défaut) : une clé absente (vieux preset) garde la valeur actuelle
    for (auto & [key, ptr] : presetFloats()) *ptr = j.value(key, *ptr);
    for (auto & [key, ptr] : presetBools()) *ptr = j.value(key, *ptr);
    turingMaskMode = std::clamp(j.value("turingMaskMode", turingMaskMode), 0, 2);

    // Anciens presets : teintes éventuellement notées en degrés (> 255) -> ramenées dans 0..255
    for (float * h : { &mapLightHue, &mapDarkHue, &genLightHue, &genMidHue, &genDarkHue,
                       &turingLightHue, &turingMidHue, &turingDarkHue }) {
        *h = wrapHue(*h);
    }

    // Les dérives auto repartent de la couleur rappelée (sinon elles filent vers l'ancienne cible)
    mapLightTargetHue = mapLightHue;
    mapDarkTargetHue = mapDarkHue;
    genLightTargetHue = genLightHue;
    genMidTargetHue = genMidHue;
    ofLogNotice("BNS-VisualGen") << "Preset " << slot << " rappele.";
}

//--------------------------------------------------------------
void ofApp::randomizeGenerativePattern(){
    nebulaScale = ofRandom(0.7f, 2.5f);
    nebulaWarp = ofRandom(1.0f, 6.0f);
    nebulaContrast = ofRandom(0.6f, 3.0f);
    evolutionSpeed = ofRandom(0.05f, 0.35f); // reste toujours lent, même au hasard

    genLightHue = ofRandom(0.0f, 255.0f);
    genLightTargetHue = genLightHue;
    genLightSat = 255; genLightBright = 255;

    genMidHue = wrapHue(genLightHue + ofRandom(60.0f, 180.0f)); genMidSat = 200; genMidBright = 230;
    genMidTargetHue = genMidHue;
    genDarkHue = wrapHue(genLightHue + 128.0f + ofRandom(-25.0f, 25.0f)); genDarkSat = 200; genDarkBright = 70;
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
void ofApp::mouseDragged(int x, int y, int /*button*/){
    if (ofGetWindowPtr() != dashboardWindowPtr) return;
    int cy = y + (int)dashScrollY;
    if (draggingSlider >= 0) applySliderDrag(draggingSlider, (float)x);
    if (draggingWheel >= 0) applyWheelDrag(draggingWheel, (float)x, (float)cy);
}

//--------------------------------------------------------------
void ofApp::mousePressed(int x, int y, int /*button*/){
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
    for (size_t i = 0; i < liveWheels.size(); i++) {
        auto & w = liveWheels[i];
        float dx = x - w.center.x;
        float dy = cy - w.center.y;
        if (dx * dx + dy * dy <= w.radius * w.radius) {
            draggingWheel = (int)i;
            applyWheelDrag((int)i, (float)x, (float)cy);
            return;
        }
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
void ofApp::mouseReleased(int, int, int){
    draggingSlider = -1;
    draggingWheel = -1;
}

//--------------------------------------------------------------
void ofApp::mouseScrolled(int, int, float, float scrollY){
    if (ofGetWindowPtr() != dashboardWindowPtr) return;
    float maxScroll = std::max(0.0f, dashContentHeight - (float)ofGetHeight());
    dashScrollY = ofClamp(dashScrollY - scrollY * 45.0f, 0.0f, maxScroll);
}
