// BNS VisualGen v2 — point d'entrée : deux fenêtres natives GLFW.
//
//   SORTIE   sans bordure, sur le 2e écran (vidéoprojecteur) s'il existe, rendu seul à 60 FPS
//   CONTRÔLE fenêtre classique sur l'écran principal, tableau de bord ImGui
//
// Options :
//   --monitor N     écran de sortie (0 = principal). Défaut : 1 si 2 écrans ou plus, sinon 0
//   --windowed      sortie en fenêtre 1280x720 (répétition sur un seul écran)
//   --render WxH    résolution interne de rendu (défaut 1920x1080)

#include "ofMain.h"
#include "ofAppGLFWWindow.h"
#include "ofApp.h"

#include <GLFW/glfw3.h>

namespace {

struct MonitorInfo {
    glm::ivec2 position;
    glm::ivec2 size;
    std::string name;
};

// Interroge GLFW AVANT la création des fenêtres (glfwInit est idempotent : OF le rappellera).
std::vector<MonitorInfo> listMonitors() {
    std::vector<MonitorInfo> result;
    if (!glfwInit()) return result;
    int count = 0;
    GLFWmonitor ** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        const GLFWvidmode * mode = glfwGetVideoMode(monitors[i]);
        if (!mode) continue;
        MonitorInfo info;
        glfwGetMonitorPos(monitors[i], &info.position.x, &info.position.y);
        info.size = { mode->width, mode->height };
        const char * name = glfwGetMonitorName(monitors[i]);
        info.name = name ? name : "?";
        result.push_back(info);
    }
    return result;
}

struct Options {
    int monitor = -1;           // -1 = automatique
    bool windowed = false;
    glm::ivec2 render{ 1920, 1080 };
};

Options parseOptions(int argc, char ** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--monitor" && i + 1 < argc) {
            o.monitor = ofToInt(argv[++i]);
        } else if (arg == "--windowed") {
            o.windowed = true;
        } else if (arg == "--render" && i + 1 < argc) {
            auto wh = ofSplitString(argv[++i], "x");
            if (wh.size() == 2) o.render = { ofToInt(wh[0]), ofToInt(wh[1]) };
        } else {
            ofLogWarning("main") << "option inconnue ignorée : " << arg;
        }
    }
    return o;
}

} // namespace

int main(int argc, char ** argv) {
    const Options options = parseOptions(argc, argv);
    const std::vector<MonitorInfo> monitors = listMonitors();
    for (size_t i = 0; i < monitors.size(); ++i) {
        ofLogNotice("main") << "écran " << i << " : " << monitors[i].name << " "
                            << monitors[i].size.x << "x" << monitors[i].size.y
                            << " @ " << monitors[i].position.x << "," << monitors[i].position.y;
    }

    int outMonitor = options.monitor;
    if (outMonitor < 0 || outMonitor >= static_cast<int>(monitors.size())) {
        outMonitor = monitors.size() >= 2 ? 1 : 0;
    }

    // --- Fenêtre SORTIE -----------------------------------------------------------------
    // Sans bordure et calée sur l'écran cible ("borderless windowed"), PAS en OF_FULLSCREEN :
    // une fenêtre GLFW en plein écran exclusif se minimise dès qu'elle perd le focus
    // (GLFW_AUTO_ICONIFY) -> le premier clic sur la GUI ferait disparaître l'image du projo.
    // GL 2.1 (renderer par défaut) : shaders GLSL 1.20 existants, et ofxImGui exige la MÊME
    // version GL dans les deux fenêtres.
    ofGLFWWindowSettings outSettings;
    outSettings.title = "BNS VisualGen - Output";
    outSettings.decorated = false;
    outSettings.resizable = false;
    outSettings.windowMode = OF_WINDOW;
    if (options.windowed || monitors.empty()) {
        outSettings.setSize(1280, 720);
        outSettings.setPosition({ 80, 80 });
    } else {
        const MonitorInfo & m = monitors[outMonitor];
        outSettings.setSize(m.size.x, m.size.y);
        outSettings.setPosition(m.position);
    }
    std::shared_ptr<ofAppBaseWindow> outputWindow = ofCreateWindow(outSettings);

    // --- Fenêtre CONTRÔLE ---------------------------------------------------------------
    // Contexte partagé avec la sortie : les TEXTURES et SHADERS sont communs (aperçu du rendu
    // dans la GUI sans copie), les FBO ne le sont pas (voir règles dans ofApp.h).
    ofGLFWWindowSettings guiSettings;
    guiSettings.title = "BNS VisualGen - Control";
    guiSettings.decorated = true;
    guiSettings.resizable = true;
    guiSettings.windowMode = OF_WINDOW;
    guiSettings.setSize(1280, 820);
    guiSettings.setPosition(monitors.empty() ? glm::ivec2{ 40, 40 }
                                             : monitors[0].position + glm::ivec2{ 40, 40 });
    guiSettings.shareContextWith = outputWindow;
    std::shared_ptr<ofAppBaseWindow> guiWindow = ofCreateWindow(guiSettings);

    // --- Moteur partagé -----------------------------------------------------------------
    auto engine = std::make_shared<bns::VisualEngine>();
    engine->renderWidth = options.render.x;
    engine->renderHeight = options.render.y;
    engine->outputWindow = outputWindow;

    auto outputApp = std::make_shared<ofApp>(engine);
    auto guiApp = std::make_shared<GuiApp>(engine);

    // ORDRE IMPORTANT : ofRunApp() déclenche setup() IMMÉDIATEMENT, dans le contexte de la
    // fenêtre concernée. La sortie d'abord : elle crée les FBO, compile et préchauffe toutes
    // les scènes ; la GUI, ensuite, peut afficher l'aperçu et la liste des scènes prêtes.
    ofRunApp(outputWindow, outputApp);
    ofRunApp(guiWindow, guiApp);

    return ofRunMainLoop();
}
