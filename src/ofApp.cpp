#include "ofApp.h"

#include <algorithm>
#include <cmath>

namespace bns {

namespace {

constexpr const char * kVertexShader = "shaders/passthrough.vert";

// Lissage à constante de temps réelle (secondes) : identique quel que soit le framerate.
void smoothTowards(float & value, float target, float attack, float release, float dt) {
    const float tau = std::max(target > value ? attack : release, 1e-4f);
    value += (target - value) * (1.f - std::exp(-dt / tau));
}

ofFboSettings fieldFboSettings(int w, int h) {
    ofFboSettings s;
    s.width = w;
    s.height = h;
    s.internalformat = GL_RGBA16F;             // champ continu : 8 bits donnerait des bords en escalier
    s.textureTarget = GL_TEXTURE_RECTANGLE_ARB;
    s.minFilter = GL_LINEAR;                   // lissé à l'agrandissement, puis quantifié net à 100 %
    s.maxFilter = GL_LINEAR;
    s.wrapModeHorizontal = GL_CLAMP_TO_EDGE;
    s.wrapModeVertical = GL_CLAMP_TO_EDGE;
    s.useDepth = false;
    s.useStencil = false;
    return s;
}

void clearFbo(ofFbo & fbo, float r = 0.f, float g = 0.f) {
    fbo.begin();
    ofClear(ofFloatColor(r, g, 0.f, 1.f));
    fbo.end();
}

} // namespace

// =====================================================================================
//  AUDIO
// =====================================================================================

AudioEngine::~AudioEngine() {
    close();
    if (fft_) {
        kiss_fftr_free(fft_);
        fft_ = nullptr;
    }
}

bool AudioEngine::setup(int deviceId) {
    close();

    // Tampons du thread audio : alloués une fois pour toutes, avant l'ouverture du flux.
    if (!fft_) {
        fft_ = kiss_fftr_alloc(kFftSize, 0, nullptr, nullptr);
        ring_.assign(kFftSize, 0.f);
        frame_.assign(kFftSize, 0.f);
        spectrum_.assign(kFftSize / 2 + 1, kiss_fft_cpx{ 0.f, 0.f });
        hann_.resize(kFftSize);
        for (int i = 0; i < kFftSize; ++i) {
            hann_[i] = 0.5f * (1.f - std::cos(TWO_PI * i / (kFftSize - 1)));
        }
    }

    // PulseAudio (ou PipeWire-Pulse) d'abord : il suit la source choisie dans les réglages
    // son du système et partage la carte. ALSA direct en secours.
    for (auto api : { ofSoundDevice::Api::PULSE, ofSoundDevice::Api::ALSA }) {
        ofSoundStreamSettings s;
        s.setApi(api);
        s.setInListener(this);
        s.sampleRate = kSampleRate;
        s.bufferSize = kBufferSize;
        s.numBuffers = 4;
        s.numInputChannels = 2;
        s.numOutputChannels = 0;
        if (deviceId >= 0) {
            for (const auto & d : stream_.getDeviceList(api)) {
                if (d.deviceID == deviceId && d.inputChannels > 0) {
                    s.setInDevice(d);
                    s.numInputChannels = std::min<int>(2, d.inputChannels);
                    break;
                }
            }
        }
        if (stream_.setup(s)) {
            deviceId_ = deviceId;
            ofLogNotice("AudioEngine") << "entrée audio ouverte ("
                                       << (api == ofSoundDevice::Api::PULSE ? "PulseAudio" : "ALSA") << ")";
            return true;
        }
    }
    ofLogError("AudioEngine") << "aucune entrée audio utilisable : visuels sans réactivité audio";
    return false;
}

void AudioEngine::close() {
    stream_.close();
}

std::vector<ofSoundDevice> AudioEngine::listInputDevices() {
    std::vector<ofSoundDevice> inputs;
    for (const auto & d : stream_.getDeviceList(ofSoundDevice::Api::PULSE)) {
        if (d.inputChannels > 0) inputs.push_back(d);
    }
    return inputs;
}

void AudioEngine::audioIn(ofSoundBuffer & buffer) {
    const int frames = static_cast<int>(buffer.getNumFrames());
    const int channels = static_cast<int>(buffer.getNumChannels());
    if (!fft_ || channels <= 0 || frames <= 0) return;

    // Downmix mono dans la fenêtre glissante (plus longue que le buffer matériel : résolution
    // fréquentielle suffisante pour isoler le kick sans augmenter la latence).
    for (int i = 0; i < frames; ++i) {
        float sum = 0.f;
        for (int c = 0; c < channels; ++c) sum += buffer.getSample(i, c);
        ring_[ringPos_] = sum / channels;
        ringPos_ = (ringPos_ + 1) % kFftSize;
    }
    for (int i = 0; i < kFftSize; ++i) {
        frame_[i] = ring_[(ringPos_ + i) % kFftSize] * hann_[i];
    }
    kiss_fftr(fft_, frame_.data(), spectrum_.data());

    const float sampleRate = buffer.getSampleRate() > 0 ? static_cast<float>(buffer.getSampleRate())
                                                        : static_cast<float>(kSampleRate);
    const float binHz = sampleRate / kFftSize;
    auto bandEnergy = [&](float loHz, float hiHz) {
        const int lo = std::max(1, static_cast<int>(loHz / binHz));
        const int hi = std::min(static_cast<int>(spectrum_.size()) - 1, static_cast<int>(hiHz / binHz));
        float sum = 0.f;
        for (int b = lo; b <= hi; ++b) {
            sum += std::sqrt(spectrum_[b].r * spectrum_[b].r + spectrum_[b].i * spectrum_[b].i);
        }
        return hi >= lo ? sum / (hi - lo + 1) : 0.f;
    };

    rawKick_.store(bandEnergy(40.f, 120.f), std::memory_order_relaxed);
    rawMid_.store(bandEnergy(250.f, 2000.f), std::memory_order_relaxed);
    rawHigh_.store(bandEnergy(4000.f, 12000.f), std::memory_order_relaxed);

    // Spectre logarithmique 30 Hz - 16 kHz pour l'affichage GUI
    for (int b = 0; b < kSpectrumBands; ++b) {
        const float lo = 30.f * std::pow(16000.f / 30.f, static_cast<float>(b) / kSpectrumBands);
        const float hi = 30.f * std::pow(16000.f / 30.f, static_cast<float>(b + 1) / kSpectrumBands);
        rawSpectrum_[b].store(bandEnergy(lo, hi), std::memory_order_relaxed);
    }
}

void AudioEngine::update(float dt) {
    const float raw[3] = {
        rawKick_.load(std::memory_order_relaxed) * settings.gain,
        rawMid_.load(std::memory_order_relaxed) * settings.gain,
        rawHigh_.load(std::memory_order_relaxed) * settings.gain,
    };

    // Auto-normalisation : chaque bande est rapportée à son maximum récent (qui redescend
    // lentement) -> même amplitude visuelle du warm-up au peak time, sans retoucher le gain.
    const float peakDecay = std::exp(-dt / std::max(settings.peakMemory, 0.1f));
    float norm[3];
    for (int i = 0; i < 3; ++i) {
        peaks_[i] = std::max(raw[i], peaks_[i] * peakDecay);
        // Porte de bruit : en silence (peak minuscule), on ne normalise pas le souffle à 1.
        norm[i] = peaks_[i] > 1e-4f ? ofClamp(raw[i] / peaks_[i], 0.f, 1.f) : 0.f;
    }

    smoothTowards(bands_.kick, norm[0], settings.kickAttack, settings.kickRelease, dt);
    smoothTowards(bands_.mid, norm[1], settings.bodyAttack, settings.bodyRelease, dt);
    smoothTowards(bands_.high, norm[2], settings.bodyAttack, settings.bodyRelease, dt);

    // Détection de kick par flux d'énergie : nettement au-dessus de la moyenne locale récente.
    const float now = ofGetElapsedTimef();
    bands_.kickOnset = norm[0] > 0.3f
                       && norm[0] > kickLocalAvg_ * settings.onsetThreshold + 0.05f
                       && (now - lastOnsetTime_) > settings.onsetRefractory;
    if (bands_.kickOnset) {
        const float interval = now - lastOnsetTime_;
        if (interval > 0.3f && interval < 1.0f) { // 60-200 BPM plausibles
            const float instant = 60.f / interval;
            bands_.bpm = bands_.bpm <= 1.f ? instant : bands_.bpm + (instant - bands_.bpm) * 0.2f;
        }
        lastOnsetTime_ = now;
        bands_.kickPulse = 1.f;
    } else {
        bands_.kickPulse *= std::exp(-dt / 0.15f);
    }
    kickLocalAvg_ += (norm[0] - kickLocalAvg_) * (1.f - std::exp(-dt / 0.5f));
    if (now - lastOnsetTime_ > 4.f) bands_.bpm = 0.f; // break : tempo inconnu
}

void AudioEngine::copySpectrum(std::array<float, kSpectrumBands> & out) const {
    float peak = 1e-6f;
    for (int b = 0; b < kSpectrumBands; ++b) {
        out[b] = rawSpectrum_[b].load(std::memory_order_relaxed);
        peak = std::max(peak, out[b]);
    }
    for (auto & v : out) v /= peak;
}

// =====================================================================================
//  PALETTES
// =====================================================================================

void PaletteManager::setupDefaults() {
    auto hex = [](int rgb) { return ofFloatColor::fromHex(rgb); };
    add({ "Acid",     { hex(0xFF00A0), hex(0xFFE600), hex(0x00F0FF), hex(0x7CFF00), hex(0xFF6A00) } });
    add({ "Spore",    { hex(0xFF3CAC), hex(0xFFB000), hex(0x8AFF00), hex(0x00E5FF), hex(0xB14DFF) } });
    add({ "Mycelium", { hex(0xFF5F1F), hex(0xFFD300), hex(0x00FFA3), hex(0xFF2EC4) } });
    add({ "UV",       { hex(0x7A00FF), hex(0xFF00FF), hex(0x00F6FF), hex(0xCCFF00) } });
    add({ "Toxic",    { hex(0x39FF14), hex(0xFFFF00), hex(0xFF073A), hex(0x00FFFF), hex(0xFF9900) } });
    add({ "Solar",    { hex(0xFFEA00), hex(0xFF7A00), hex(0xFF0055), hex(0xFF00D4) } });
    current_ = 0;
}

bool PaletteManager::isVivid(const ofFloatColor & c) {
    return c.getSaturation() >= 0.55f && c.getBrightness() >= 0.70f;
}

bool PaletteManager::add(const Palette & palette) {
    if (palette.colors.size() < 2 || palette.colors.size() > kMaxColors) {
        ofLogError("PaletteManager") << "palette \"" << palette.name << "\" refusée : 2 à "
                                     << kMaxColors << " couleurs attendues";
        return false;
    }
    for (const auto & c : palette.colors) {
        if (!isVivid(c)) {
            ofLogError("PaletteManager") << "palette \"" << palette.name << "\" refusée : couleur "
                                         << c << " trop sombre ou terne (pas de noir, pas de contour)";
            return false;
        }
    }
    palettes_.push_back(palette);
    return true;
}

void PaletteManager::select(int index) {
    if (index >= 0 && index < static_cast<int>(palettes_.size())) current_ = index;
}

void PaletteManager::apply(ofShader & shader, float bandOffset) const {
    std::array<float, kMaxColors * 3> flat{};
    const auto & colors = current().colors;
    for (size_t i = 0; i < kMaxColors; ++i) {
        const ofFloatColor & c = colors[i % colors.size()]; // cases vides : répétition, jamais noir
        flat[i * 3 + 0] = c.r;
        flat[i * 3 + 1] = c.g;
        flat[i * 3 + 2] = c.b;
    }
    shader.setUniform3fv("u_palette", flat.data(), kMaxColors);
    shader.setUniform1i("u_paletteSize", static_cast<int>(colors.size()));
    shader.setUniform1f("u_bands", static_cast<float>(std::max(bands, 2)));
    shader.setUniform1f("u_bandOffset", bandOffset);
}

// =====================================================================================
//  SCÈNES
// =====================================================================================

ShaderScene::ShaderScene(std::string name, std::string fragmentPath)
    : Scene(std::move(name)), fragmentPath_(std::move(fragmentPath)) {}

bool ShaderScene::load(int, int) {
    ready_ = shader_.load(kVertexShader, fragmentPath_) && shader_.isLoaded();
    if (!ready_) ofLogError("Scene") << name_ << " : shader inutilisable (" << fragmentPath_ << ")";
    return ready_;
}

void ShaderScene::update(const SceneContext & ctx) {
    const float audioDrive = ctx.audio ? ctx.audio->mid * ctx.audioAmount : 0.f;
    localTime_ += ctx.dt * speed_ * (1.f + 0.6f * audioDrive);
}

void ShaderScene::renderField(const SceneContext & ctx) {
    const AudioBands none;
    const AudioBands & a = ctx.audio ? *ctx.audio : none;
    shader_.begin();
    shader_.setUniform1f("u_time", localTime_);
    shader_.setUniform2f("u_resolution", ctx.resolution);
    shader_.setUniform1f("u_kick", a.kick * ctx.audioAmount);
    shader_.setUniform1f("u_mid", a.mid * ctx.audioAmount);
    shader_.setUniform1f("u_high", a.high * ctx.audioAmount);
    shader_.setUniform1f("u_pulse", a.kickPulse * ctx.audioAmount);
    shader_.setUniform1f("u_speed", speed_);
    shader_.setUniform1f("u_scale", scale_);
    shader_.setUniform1f("u_complexity", complexity_);
    shader_.setUniform1f("u_audio", ctx.audioAmount);
    ofDrawRectangle(0, 0, ctx.resolution.x, ctx.resolution.y);
    shader_.end();
}

void ShaderScene::drawGui() {
    ImGui::PushID(name_.c_str());
    ImGui::SliderFloat("Vitesse", &speed_, 0.f, 3.f);
    ImGui::SliderFloat("Echelle", &scale_, 0.3f, 3.f);
    ImGui::SliderFloat("Complexite", &complexity_, 0.f, 1.f);
    ImGui::PopID();
}

// --- Morphogen ------------------------------------------------------------------------

MorphogenScene::MorphogenScene() : Scene("Morphogen") {}

bool MorphogenScene::load(int fieldWidth, int fieldHeight) {
    simWidth_ = std::max(160, fieldWidth / 2);
    simHeight_ = std::max(90, fieldHeight / 2);

    const bool stepOk = stepShader_.load(kVertexShader, "shaders/scenes/morphogen_step.frag");
    const bool fieldOk = fieldShader_.load(kVertexShader, "shaders/scenes/morphogen_field.frag");

    ofFboSettings s = fieldFboSettings(simWidth_, simHeight_);
    s.internalformat = GL_RGBA32F; // la simulation a besoin de précision (petites valeurs de V)
    for (auto & fbo : sim_) fbo.allocate(s);
    seed();

    ready_ = stepOk && fieldOk;
    if (!ready_) ofLogError("Scene") << "Morphogen : shaders inutilisables";
    return ready_;
}

void MorphogenScene::seed() {
    // U = 1 partout, V = 0, plus des germes de V répartis sur toute la surface (croissance
    // immédiate partout, façon mycélium, plutôt qu'une lente invasion depuis le centre).
    for (auto & fbo : sim_) {
        fbo.begin();
        ofClear(ofFloatColor(1.f, 0.f, 0.f, 1.f));
        ofSetColor(255, 255, 0);
        for (int i = 0; i < 40; ++i) {
            const float size = ofRandom(3.f, 9.f);
            ofDrawRectangle(ofRandom(simWidth_), ofRandom(simHeight_), size, size);
        }
        fbo.end();
    }
    src_ = 0;
}

void MorphogenScene::enter() {
    if (reseedOnEnter_) reseedRequested_ = true;
}

void MorphogenScene::update(const SceneContext & ctx) {
    if (reseedRequested_) {
        seed();
        reseedRequested_ = false;
    }
    if (ctx.dt <= 0.f) return; // figé

    // Les kicks plantent de nouveaux germes : la croissance repart au rythme du morceau.
    if (ctx.audio && ctx.audio->kickOnset && ofRandomuf() < kickInjection_ * ctx.audioAmount) {
        sim_[src_].begin();
        ofSetColor(255, 255, 0);
        for (int i = 0; i < 3; ++i) {
            ofDrawCircle(ofRandom(simWidth_), ofRandom(simHeight_), ofRandom(2.f, 5.f));
        }
        sim_[src_].end();
    }

    for (int i = 0; i < stepsPerFrame_; ++i) {
        ofFbo & src = sim_[src_];
        ofFbo & dst = sim_[1 - src_];
        dst.begin();
        stepShader_.begin();
        stepShader_.setUniform1f("feed", feed_);
        stepShader_.setUniform1f("kill", kill_);
        stepShader_.setUniform1f("du", 1.f);
        stepShader_.setUniform1f("dv", diffusionV_);
        stepShader_.setUniform1f("u_time", ofGetElapsedTimef() + i * 0.013f);
        ofSetColor(255);
        src.draw(0, 0);
        stepShader_.end();
        dst.end();
        src_ = 1 - src_;
    }
}

void MorphogenScene::renderField(const SceneContext & ctx) {
    fieldShader_.begin();
    fieldShader_.setUniform1f("u_pulse", ctx.audio ? ctx.audio->kickPulse * ctx.audioAmount : 0.f);
    ofSetColor(255);
    sim_[src_].draw(0, 0, ctx.resolution.x, ctx.resolution.y);
    fieldShader_.end();
}

void MorphogenScene::drawGui() {
    ImGui::PushID("morphogen");
    ImGui::SliderFloat("Feed", &feed_, 0.010f, 0.090f, "%.4f");
    ImGui::SliderFloat("Kill", &kill_, 0.030f, 0.080f, "%.4f");
    ImGui::SliderFloat("Diffusion V", &diffusionV_, 0.2f, 1.f);
    ImGui::SliderInt("Pas / frame", &stepsPerFrame_, 1, 20);
    ImGui::SliderFloat("Germes sur kick", &kickInjection_, 0.f, 1.f);
    ImGui::Checkbox("Reensemencer a l'entree", &reseedOnEnter_);
    if (ImGui::Button("Reensemencer")) reseedRequested_ = true; // traité dans update() (contexte sortie)
    ImGui::PopID();
}

// =====================================================================================
//  WEBCAM
// =====================================================================================

void WebcamLayer::setup(int fieldWidth, int fieldHeight) {
    width_ = fieldWidth;
    height_ = fieldHeight;
    for (auto & fbo : feedback_) {
        fbo.allocate(fieldFboSettings(width_, height_));
        clearFbo(fbo);
    }
    if (!feedbackShader_.load(kVertexShader, "shaders/webcam_feedback.frag")) {
        ofLogError("WebcamLayer") << "shader de feedback inutilisable : couche caméra désactivée";
        params.enabled = false;
        return;
    }

    devices_ = grabber_.listDevices();
    for (const auto & d : devices_) {
        if (d.bAvailable) {
            openDevice(d.id);
            break;
        }
    }
    if (!opened_) ofLogWarning("WebcamLayer") << "aucune caméra : couche caméra inactive";
}

void WebcamLayer::openDevice(int deviceId) {
    grabber_.close();
    grabber_.setDeviceID(deviceId);
    grabber_.setDesiredFrameRate(30);
    opened_ = grabber_.setup(640, 480);
    deviceId_ = opened_ ? deviceId : -1;
    ofLogNotice("WebcamLayer") << "caméra " << deviceId << (opened_ ? " ouverte" : " : échec d'ouverture");
}

void WebcamLayer::close() {
    grabber_.close();
    opened_ = false;
}

void WebcamLayer::update(float dt, const AudioBands & audio, float audioAmount) {
    if (pendingDevice_ >= 0) {
        openDevice(pendingDevice_);
        pendingDevice_ = -1;
    }
    if (!isActive() || !feedbackShader_.isLoaded()) return;

    grabber_.update();
    if (!grabber_.getTexture().isAllocated()) return;

    // Paramètres exprimés "par seconde" / "à 60 FPS" : la boucle tourne pareil à 30 ou 60 FPS.
    const float frames60 = dt * 60.f;
    float zoom = std::pow(params.zoom, frames60);
    zoom *= 1.f + audio.kickPulse * params.kickZoom * audioAmount;
    const float angle = ofDegToRad(params.rotation) * dt;
    const float decay = std::pow(params.feedback, frames60);

    ofFbo & src = feedback_[src_];
    ofFbo & dst = feedback_[1 - src_];
    dst.begin();
    feedbackShader_.begin();
    feedbackShader_.setUniformTexture("u_prev", src.getTexture(), 1);
    feedbackShader_.setUniformTexture("u_cam", grabber_.getTexture(), 2);
    feedbackShader_.setUniform2f("u_size", glm::vec2(width_, height_));
    feedbackShader_.setUniform2f("u_camSize", glm::vec2(grabber_.getWidth(), grabber_.getHeight()));
    feedbackShader_.setUniform1f("u_zoom", zoom);
    feedbackShader_.setUniform1f("u_angle", angle);
    feedbackShader_.setUniform1f("u_decay", decay);
    feedbackShader_.setUniform1f("u_gain", params.gain);
    feedbackShader_.setUniform1i("u_mirror", params.mirror ? 1 : 0);
    ofSetColor(255);
    ofDrawRectangle(0, 0, width_, height_);
    feedbackShader_.end();
    dst.end();
    src_ = 1 - src_;
}

// =====================================================================================
//  SCENE MANAGER
// =====================================================================================

void SceneManager::add(std::unique_ptr<Scene> scene) {
    scenes_.push_back(std::move(scene));
}

bool SceneManager::setup(int outputWidth, int outputHeight, float fieldScale, const PaletteManager & palettes) {
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    fieldWidth_ = std::max(64, static_cast<int>(std::round(outputWidth * fieldScale)));
    fieldHeight_ = std::max(36, static_cast<int>(std::round(outputHeight * fieldScale)));

    fieldActive_.allocate(fieldFboSettings(fieldWidth_, fieldHeight_));
    fieldPrevious_.allocate(fieldFboSettings(fieldWidth_, fieldHeight_));
    clearFbo(fieldActive_);
    clearFbo(fieldPrevious_);

    ofFboSettings out;
    out.width = outputWidth_;
    out.height = outputHeight_;
    out.internalformat = GL_RGBA8;
    out.textureTarget = GL_TEXTURE_2D; // affichable par ImGui::Image dans la fenêtre GUI
    out.minFilter = GL_LINEAR;
    out.maxFilter = GL_LINEAR;
    out.useDepth = false;
    output_.allocate(out);

    if (!compositeShader_.load(kVertexShader, "shaders/composite.frag")) {
        ofLogFatalError("SceneManager") << "shaders/composite.frag inutilisable : rendu impossible";
        return false;
    }

    SceneContext ctx;
    ctx.dt = 1.f / 60.f;
    ctx.resolution = { fieldWidth_, fieldHeight_ };
    const AudioBands silence;
    ctx.audio = &silence;

    int firstReady = -1;
    for (size_t i = 0; i < scenes_.size(); ++i) {
        Scene & scene = *scenes_[i];
        const uint64_t t0 = ofGetElapsedTimeMicros();
        if (!scene.load(fieldWidth_, fieldHeight_)) continue;
        warmup(static_cast<int>(i), ctx, palettes);
        ofLogNotice("SceneManager") << scene.name() << " prête ("
                                    << (ofGetElapsedTimeMicros() - t0) / 1000 << " ms de chargement)";
        if (firstReady < 0) firstReady = static_cast<int>(i);
    }
    if (firstReady < 0) {
        ofLogFatalError("SceneManager") << "aucune scène utilisable";
        return false;
    }
    active_ = firstReady; // warmup() a déplacé active_ : on repart de la première scène prête
    scenes_[active_]->enter();
    return true;
}

void SceneManager::warmup(int index, const SceneContext & ctx, const PaletteManager & palettes) {
    // Un vrai rendu hors écran, avec tous les uniforms : force le pilote à finir la compilation
    // et à créer ses objets internes maintenant, plutôt qu'au premier affichage en plein set.
    // render() dessine la scène active : on rend temporairement active la scène préchauffée.
    active_ = index;
    scenes_[index]->update(ctx);
    render(ctx, palettes, 0.f, nullptr); // champ de la scène + passe composite
    glFinish();
}

void SceneManager::requestScene(int index) {
    if (index >= 0 && index < sceneCount() && scenes_[index]->isReady() && index != active_) {
        pending_ = index;
    }
}

void SceneManager::requestNext() {
    for (int step = 1; step < sceneCount(); ++step) {
        const int candidate = (active_ + step) % sceneCount();
        if (scenes_[candidate]->isReady()) {
            requestScene(candidate);
            return;
        }
    }
}

void SceneManager::applyPendingSwitch() {
    if (pending_ < 0) return;
    const uint64_t t0 = ofGetElapsedTimeMicros();
    scenes_[active_]->leave();
    previous_ = (transition == TransitionMode::Dither && transitionDuration > 0.f) ? active_ : -1;
    active_ = std::exchange(pending_, -1);
    scenes_[active_]->enter(); // O(1) : ni allocation ni compilation (tout est fait dans setup)
    transitionT_ = previous_ >= 0 ? 0.f : 1.f;
    lastSwitchCostMs_ = (ofGetElapsedTimeMicros() - t0) / 1000.f;
}

void SceneManager::update(float dt, const SceneContext & ctx) {
    applyPendingSwitch();
    if (previous_ >= 0) {
        transitionT_ += dt / std::max(transitionDuration, 1e-3f); // temps réel, même figé
        if (transitionT_ >= 1.f) {
            transitionT_ = 1.f;
            previous_ = -1;
        }
    }
    scenes_[active_]->update(ctx);
    if (previous_ >= 0) scenes_[previous_]->update(ctx); // la sortante vit jusqu'à la fin du fondu tramé
}

void SceneManager::render(const SceneContext & ctx, const PaletteManager & palettes, float bandOffset,
                          const WebcamLayer * webcam) {
    ofDisableAlphaBlending(); // champs = données, pas d'image : aucun mélange

    fieldActive_.begin();
    scenes_[active_]->renderField(ctx);
    fieldActive_.end();

    const bool transitioning = previous_ >= 0;
    if (transitioning) {
        fieldPrevious_.begin();
        scenes_[previous_]->renderField(ctx);
        fieldPrevious_.end();
    }

    const bool camOn = webcam && webcam->isActive();

    output_.begin();
    compositeShader_.begin();
    compositeShader_.setUniformTexture("u_field", fieldActive_.getTexture(), 1);
    compositeShader_.setUniformTexture("u_prevField", (transitioning ? fieldPrevious_ : fieldActive_).getTexture(), 2);
    compositeShader_.setUniformTexture("u_camField", camOn ? webcam->field() : fieldActive_.getTexture(), 3);
    compositeShader_.setUniform2f("u_fieldSize", glm::vec2(fieldWidth_, fieldHeight_));
    compositeShader_.setUniform2f("u_outSize", glm::vec2(outputWidth_, outputHeight_));
    compositeShader_.setUniform1f("u_mix", transitioning ? transitionT_ : 1.f);
    compositeShader_.setUniform1f("u_block", static_cast<float>(std::max(1, ditherBlockSize)));
    compositeShader_.setUniform1i("u_camMode", camOn ? static_cast<int>(webcam->params.mode) : 0);
    compositeShader_.setUniform1f("u_camAmount", camOn ? webcam->params.amount : 0.f);
    compositeShader_.setUniform1f("u_camThreshold", camOn ? webcam->params.threshold : 1.f);
    palettes.apply(compositeShader_, bandOffset);
    ofSetColor(255);
    ofDrawRectangle(0, 0, outputWidth_, outputHeight_);
    compositeShader_.end();
    output_.end();
}

} // namespace bns

// =====================================================================================
//  FENÊTRE SORTIE
// =====================================================================================

void ofApp::setup() {
    ofSetWindowTitle("BNS VisualGen - Output");
    ofSetVerticalSync(true); // c'est la synchro écran de la SORTIE qui cadence tout à 60 Hz
    ofSetFrameRate(0);       // pas de minuteur logiciel : il endormirait aussi la fenêtre GUI
    ofSetEscapeQuitsApp(false); // Échap par erreur en plein set ne doit PAS couper l'image
    ofHideCursor();

    auto & e = *engine_;
    e.palettes.setupDefaults();

    e.scenes.add(std::make_unique<bns::MorphogenScene>());
    e.scenes.add(std::make_unique<bns::ShaderScene>("Fluid", "shaders/scenes/fluid.frag"));
    e.scenes.add(std::make_unique<bns::ShaderScene>("Mandala", "shaders/scenes/mandala.frag"));
    e.scenes.add(std::make_unique<bns::ShaderScene>("Spore", "shaders/scenes/spore.frag"));

    if (!e.scenes.setup(e.renderWidth, e.renderHeight, e.fieldScale, e.palettes)) {
        ofLogFatalError("ofApp") << "moteur de scènes inutilisable (voir erreurs ci-dessus) : arrêt";
        ofExit(1);
        return;
    }
    const glm::vec2 field = e.scenes.fieldSize(); // même résolution que les champs de scène
    e.webcam.setup(static_cast<int>(field.x), static_cast<int>(field.y));
    e.audio.setup(-1);
}

void ofApp::update() {
    auto & e = *engine_;
    const float dt = ofClamp(static_cast<float>(ofGetLastFrameTime()), 0.f, 0.1f); // pas de saut après un gel

    e.audio.update(dt);
    const bns::AudioBands & audio = e.audio.bands();

    const float sceneDt = e.live.freeze ? 0.f : dt * e.live.masterSpeed;
    e.sceneTime += sceneDt;

    // Défilement des aplats : continu (cycleSpeed) + un saut d'une bande par kick (net, jamais fondu)
    e.bandPhase += e.palettes.cycleSpeed * sceneDt / std::max(e.palettes.bands, 2);
    if (e.paletteStepOnKick && audio.kickOnset && !e.live.freeze) {
        e.bandPhase += 1.f / std::max(e.palettes.bands, 2);
    }
    e.bandPhase -= std::floor(e.bandPhase);

    ctx_.time = e.sceneTime;
    ctx_.dt = sceneDt;
    ctx_.audio = &audio;
    ctx_.audioAmount = e.live.audioAmount;
    ctx_.resolution = e.scenes.fieldSize();

    if (!e.live.freeze) e.webcam.update(dt, audio, e.live.audioAmount);
    e.scenes.update(dt, ctx_);

    e.outputFps = ofGetFrameRate();
    e.outputFrameMs = dt * 1000.f;
}

void ofApp::draw() {
    auto & e = *engine_;
    e.scenes.render(ctx_, e.palettes, e.bandPhase, &e.webcam);
    ofSetColor(255);
    e.scenes.output().draw(0, 0, ofGetWidth(), ofGetHeight());
}

void ofApp::exit() {
    engine_->audio.close();
    engine_->webcam.close();
}

void ofApp::keyPressed(int key) {
    // Secours si la fenêtre GUI est inaccessible (écran de contrôle perdu en plein set)
    if (key >= '1' && key <= '9') engine_->scenes.requestScene(key - '1');
    if (key == 'n') engine_->scenes.requestNext();
}

// =====================================================================================
//  FENÊTRE CONTRÔLE
// =====================================================================================

void GuiApp::setup() {
    ofSetWindowTitle("BNS VisualGen - Control");
    ofSetVerticalSync(false); // sinon 2 attentes d'écran par tour de boucle -> 30 FPS sur la sortie
    ofSetFrameRate(0);
    // Mise en page fixe (recalculée à chaque frame) : rien à sauvegarder, pas de imgui.ini
    gui_.setup(nullptr, true, ImGuiConfigFlags_None, false);
    // Police terminal de la charte BNS (vérifiée avant : ImGui s'arrête sur un fichier absent en Debug)
    const std::string font = "fonts/terminal-bold.ttf";
    if (ofFile::doesFileExist(font)) gui_.addFont(font, 17.f, nullptr, nullptr, true);
    applyTheme();
    audioDevices_ = engine_->audio.listInputDevices();
}

void GuiApp::applyTheme() {
    // Charte BNS : fond noir, texte vert phosphore, accents jaune acide / rose fluo
    ImGuiStyle & st = ImGui::GetStyle();
    st.WindowRounding = 0.f;
    st.FrameRounding = 2.f;
    st.GrabMinSize = 14.f;
    st.FramePadding = { 8.f, 5.f };
    st.ItemSpacing = { 8.f, 7.f };
    ImVec4 * c = st.Colors;
    const ImVec4 black{ 0.03f, 0.03f, 0.03f, 1.f }, panel{ 0.08f, 0.08f, 0.08f, 1.f };
    const ImVec4 green{ 0.24f, 1.f, 0.51f, 1.f }, acid{ 0.92f, 1.f, 0.12f, 1.f }, pink{ 1.f, 0.f, 0.59f, 1.f };
    c[ImGuiCol_Text] = green;
    c[ImGuiCol_WindowBg] = black;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = { 0.24f, 1.f, 0.51f, 0.35f };
    c[ImGuiCol_FrameBg] = { 0.14f, 0.14f, 0.14f, 1.f };
    c[ImGuiCol_FrameBgHovered] = { 0.2f, 0.2f, 0.2f, 1.f };
    c[ImGuiCol_FrameBgActive] = { 0.25f, 0.25f, 0.25f, 1.f };
    c[ImGuiCol_SliderGrab] = acid;
    c[ImGuiCol_SliderGrabActive] = pink;
    c[ImGuiCol_CheckMark] = acid;
    c[ImGuiCol_Button] = { 0.14f, 0.14f, 0.14f, 1.f };
    c[ImGuiCol_ButtonHovered] = { 1.f, 0.f, 0.59f, 0.6f };
    c[ImGuiCol_ButtonActive] = pink;
    c[ImGuiCol_Header] = { 0.24f, 1.f, 0.51f, 0.25f };
    c[ImGuiCol_HeaderHovered] = { 0.24f, 1.f, 0.51f, 0.4f };
    c[ImGuiCol_HeaderActive] = { 0.24f, 1.f, 0.51f, 0.55f };
    c[ImGuiCol_PlotHistogram] = acid;
    c[ImGuiCol_Separator] = { 0.24f, 1.f, 0.51f, 0.35f };
}

void GuiApp::draw() {
    ofBackground(8);
    gui_.begin();

    ImGui::SetNextWindowPos({ 0.f, 0.f });
    ImGui::SetNextWindowSize({ static_cast<float>(ofGetWidth()), static_cast<float>(ofGetHeight()) });
    ImGui::Begin("BNS VisualGen", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float statusH = ImGui::GetFrameHeightWithSpacing() + 6.f;
    const float leftW = std::max(360.f, ImGui::GetContentRegionAvail().x * 0.42f);

    ImGui::BeginChild("left", { leftW, -statusH }, ImGuiChildFlags_Borders);
    drawSceneSwitcher();
    drawPalettePanel();
    drawMasterPanel();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("right", { 0.f, -statusH }, ImGuiChildFlags_Borders);
    drawPreview();
    drawAudioPanel();
    drawWebcamPanel();
    ImGui::EndChild();

    drawStatusBar();
    ImGui::End();
    gui_.end();
}

void GuiApp::drawSceneSwitcher() {
    auto & scenes = engine_->scenes;
    ImGui::SeparatorText("SCENES  [1-9]");

    const int n = scenes.sceneCount();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float btnW = (ImGui::GetContentRegionAvail().x - gap) / 2.f;
    for (int i = 0; i < n; ++i) {
        bns::Scene & scene = scenes.scene(i);
        const bool active = (i == scenes.activeIndex());
        if (i % 2) ImGui::SameLine();
        ImGui::BeginDisabled(!scene.isReady());
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.92f, 1.f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 1.f));
        }
        const std::string label = ofToString(i + 1) + "  " + scene.name() + (scene.isReady() ? "" : " (erreur)");
        if (ImGui::Button(label.c_str(), { btnW, 56.f })) scenes.requestScene(i);
        if (active) ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
    }

    int mode = static_cast<int>(scenes.transition);
    ImGui::RadioButton("Coupe", &mode, static_cast<int>(bns::TransitionMode::Cut));
    ImGui::SameLine();
    ImGui::RadioButton("Trame", &mode, static_cast<int>(bns::TransitionMode::Dither));
    scenes.transition = static_cast<bns::TransitionMode>(mode);
    if (scenes.transition == bns::TransitionMode::Dither) {
        ImGui::SliderFloat("Duree (s)", &scenes.transitionDuration, 0.1f, 3.f);
        ImGui::SliderInt("Taille trame", &scenes.ditherBlockSize, 1, 32);
    }

    if (ImGui::TreeNodeEx("Reglages de la scene", ImGuiTreeNodeFlags_DefaultOpen)) {
        scenes.scene(scenes.activeIndex()).drawGui();
        ImGui::TreePop();
    }
}

void GuiApp::drawPalettePanel() {
    auto & pal = engine_->palettes;
    ImGui::SeparatorText("PALETTE  [espace]");
    const auto & all = pal.all();
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable("##pal", pal.currentIndex() == i, 0, { 0.f, 24.f })) pal.select(i);
        ImGui::SameLine(8.f);
        for (size_t k = 0; k < all[i].colors.size(); ++k) {
            const auto & c = all[i].colors[k];
            ImGui::ColorButton("##c", ImVec4(c.r, c.g, c.b, 1.f),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, { 34.f, 20.f });
            ImGui::SameLine(0.f, 2.f);
        }
        ImGui::SameLine(0.f, 10.f);
        ImGui::TextUnformatted(all[i].name.c_str());
        ImGui::PopID();
    }
    ImGui::SliderInt("Aplats", &pal.bands, 2, 16);
    ImGui::SliderFloat("Defilement", &pal.cycleSpeed, 0.f, 4.f, "%.2f bandes/s");
    ImGui::Checkbox("Saut d'aplat sur chaque kick", &engine_->paletteStepOnKick);
}

void GuiApp::drawMasterPanel() {
    auto & live = engine_->live;
    ImGui::SeparatorText("MASTER");
    ImGui::SliderFloat("Vitesse globale", &live.masterSpeed, 0.f, 3.f);
    ImGui::SliderFloat("Reactivite audio", &live.audioAmount, 0.f, 2.f);
    ImGui::Checkbox("FREEZE  [f]", &live.freeze);
}

void GuiApp::drawPreview() {
    ImGui::SeparatorText("SORTIE");
    const ofTexture & tex = engine_->scenes.output().getTexture();
    if (!tex.isAllocated()) return;
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = w * tex.getHeight() / tex.getWidth();
    // Texture partagée entre les contextes : aucune copie. uv inversés : repère GL (origine en bas).
    const auto id = static_cast<ImTextureID>(tex.getTextureData().textureID);
    ImGui::Image(ImTextureRef(id), { w, h }, { 0.f, 1.f }, { 1.f, 0.f });
}

void GuiApp::drawAudioPanel() {
    auto & audio = engine_->audio;
    const bns::AudioBands & b = audio.bands();
    ImGui::SeparatorText("AUDIO");

    const std::string current = audio.currentDeviceId() < 0 ? "Entree par defaut du systeme"
                                                            : "Peripherique " + ofToString(audio.currentDeviceId());
    if (ImGui::BeginCombo("Entree", current.c_str())) {
        if (ImGui::Selectable("Entree par defaut du systeme", audio.currentDeviceId() < 0)) audio.setup(-1);
        for (const auto & d : audioDevices_) {
            if (ImGui::Selectable(d.name.c_str(), d.deviceID == audio.currentDeviceId())) audio.setup(d.deviceID);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Rafraichir")) audioDevices_ = audio.listInputDevices();

    ImGui::ProgressBar(b.kick, { -1.f, 0.f }, b.kickPulse > 0.5f ? "KICK" : "kick");
    ImGui::ProgressBar(b.mid, { -1.f, 0.f }, "mid");
    ImGui::ProgressBar(b.high, { -1.f, 0.f }, "high");
    ImGui::Text("Tempo : %s", b.bpm > 1.f ? (ofToString(static_cast<int>(std::round(b.bpm))) + " BPM").c_str() : "--");

    audio.copySpectrum(spectrum_);
    ImGui::PlotHistogram("##spectrum", spectrum_.data(), static_cast<int>(spectrum_.size()), 0, nullptr, 0.f, 1.f,
                         { -1.f, 60.f });

    if (ImGui::TreeNode("Reglages analyse")) {
        auto & s = audio.settings;
        ImGui::SliderFloat("Gain", &s.gain, 0.1f, 8.f);
        ImGui::SliderFloat("Kick attaque (s)", &s.kickAttack, 0.001f, 0.1f, "%.3f");
        ImGui::SliderFloat("Kick retombee (s)", &s.kickRelease, 0.02f, 1.f, "%.3f");
        ImGui::SliderFloat("Mid/High attaque (s)", &s.bodyAttack, 0.005f, 0.5f, "%.3f");
        ImGui::SliderFloat("Mid/High retombee (s)", &s.bodyRelease, 0.05f, 2.f, "%.3f");
        ImGui::SliderFloat("Seuil kick", &s.onsetThreshold, 1.05f, 3.f);
        ImGui::TreePop();
    }
}

void GuiApp::drawWebcamPanel() {
    auto & cam = engine_->webcam;
    auto & p = cam.params;
    ImGui::SeparatorText("WEBCAM  [c]");
    ImGui::Checkbox("Active", &p.enabled);

    if (cam.devices().empty()) {
        ImGui::TextColored({ 1.f, 0.3f, 0.3f, 1.f }, "Aucune camera detectee");
        return;
    }
    std::string current = "(aucune)";
    for (const auto & d : cam.devices()) {
        if (d.id == cam.currentDeviceId()) current = d.deviceName;
    }
    if (ImGui::BeginCombo("Camera", current.c_str())) {
        for (const auto & d : cam.devices()) {
            if (!d.bAvailable) continue;
            ImGui::PushID(d.id);
            if (ImGui::Selectable(d.deviceName.c_str(), d.id == cam.currentDeviceId())) cam.requestDevice(d.id);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    int mode = static_cast<int>(p.mode);
    ImGui::RadioButton("Silhouette", &mode, static_cast<int>(bns::CamMode::Mask));
    ImGui::SameLine();
    ImGui::RadioButton("Ajout", &mode, static_cast<int>(bns::CamMode::Add));
    ImGui::SameLine();
    ImGui::RadioButton("Camera seule", &mode, static_cast<int>(bns::CamMode::Solo));
    p.mode = static_cast<bns::CamMode>(mode);

    ImGui::SliderFloat("Intensite", &p.amount, 0.f, 1.f);
    if (p.mode == bns::CamMode::Mask) ImGui::SliderFloat("Seuil silhouette", &p.threshold, 0.f, 1.f);
    ImGui::SliderFloat("Gain camera", &p.gain, 0.2f, 3.f);
    ImGui::Checkbox("Miroir", &p.mirror);
    ImGui::SliderFloat("Remanence", &p.feedback, 0.f, 0.995f, "%.3f");
    ImGui::SliderFloat("Zoom boucle", &p.zoom, 0.97f, 1.05f, "%.3f");
    ImGui::SliderFloat("Rotation (deg/s)", &p.rotation, -90.f, 90.f);
    ImGui::SliderFloat("Zoom sur kick", &p.kickZoom, 0.f, 0.2f);
}

void GuiApp::drawStatusBar() {
    const auto & e = *engine_;
    const bool slow = e.outputFps < 57.f;
    ImGui::TextColored(slow ? ImVec4(1.f, 0.3f, 0.3f, 1.f) : ImVec4(0.24f, 1.f, 0.51f, 1.f),
                       "SORTIE %.1f FPS (%.1f ms)", e.outputFps, e.outputFrameMs);
    ImGui::SameLine(0.f, 24.f);
    ImGui::Text("Bascule : %.2f ms", e.scenes.lastSwitchCostMs());
    ImGui::SameLine(0.f, 24.f);
    ImGui::Text("Rendu %dx%d, champs x%.2f", e.renderWidth, e.renderHeight, e.fieldScale);
    ImGui::SameLine(0.f, 24.f);
    ImGui::Text("GUI %.0f FPS", ofGetFrameRate());
}

void GuiApp::exit() {
    gui_.exit();
}

void GuiApp::keyPressed(int key) {
    if (ImGui::GetIO().WantTextInput) return; // l'opérateur tape dans un champ texte
    auto & e = *engine_;
    if (key >= '1' && key <= '9') e.scenes.requestScene(key - '1');
    else if (key == 'n') e.scenes.requestNext();
    else if (key == ' ') e.palettes.select((e.palettes.currentIndex() + 1) % static_cast<int>(e.palettes.all().size()));
    else if (key == 'f') e.live.freeze = !e.live.freeze;
    else if (key == 'c') e.webcam.params.enabled = !e.webcam.params.enabled;
}
