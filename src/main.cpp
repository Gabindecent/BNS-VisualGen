#include "ofMain.h"
#include "ofApp.h"

int main( ){
    ofGLFWWindowSettings settings;
    
    // --- Fenêtre 1 : Output Visuel (Vidéoprojecteur) ---
    settings.setSize(1920, 1080);
    settings.setPosition(glm::vec2(0, 0));
    settings.resizable = true;
    settings.windowMode = OF_WINDOW; // Pourra être passé en OF_FULLSCREEN plus tard
    settings.title = "BNS VisualGen - Sortie";
    shared_ptr<ofAppBaseWindow> outputWindow = ofCreateWindow(settings);
    
    // --- Fenêtre 2 : Dashboard VJ (Contrôle) ---
    // Grande fenêtre, redimensionnable : le dashboard custom recalcule tout son layout
    // depuis la taille réelle de la fenêtre à chaque frame (vrai espace dynamique).
    settings.setSize(900, 1000);
    settings.setPosition(glm::vec2(60, 20));
    settings.resizable = true;
    settings.windowMode = OF_WINDOW;
    settings.title = "BNS VisualGen - Dashboard";
    shared_ptr<ofAppBaseWindow> dashboardWindow = ofCreateWindow(settings);

    shared_ptr<ofApp> mainApp(new ofApp);
    mainApp->dashboardWindowPtr = dashboardWindow.get();

    // On liera les événements de la fenêtre dashboard à une méthode spécifique de ofApp.
    // IMPORTANT : ofRunApp() ne branche les événements souris/clavier QUE sur la fenêtre qu'on
    // lui passe (ici outputWindow). Sans ces lignes, la fenêtre Dashboard ne reçoit jamais le
    // moindre clic -> c'est pour ça qu'aucun bouton du dashboard custom ne répondait.
    ofAddListener(dashboardWindow->events().draw, mainApp.get(), &ofApp::drawDashboard);
    // On accroche explicitement la surcharge ofBaseApp::xxx(ofMouseEventArgs&) : c'est celle que
    // l'event attend, et elle rappelle en interne notre xxx(int,int,int) via dispatch virtuel.
    // ofBaseApp a 2 surcharges de chaque nom -> il faut lever l'ambiguïté avec un cast explicite.
    ofAddListener(dashboardWindow->events().mousePressed, mainApp.get(), static_cast<void (ofApp::*)(ofMouseEventArgs&)>(&ofBaseApp::mousePressed));
    ofAddListener(dashboardWindow->events().mouseDragged, mainApp.get(), static_cast<void (ofApp::*)(ofMouseEventArgs&)>(&ofBaseApp::mouseDragged));
    ofAddListener(dashboardWindow->events().mouseReleased, mainApp.get(), static_cast<void (ofApp::*)(ofMouseEventArgs&)>(&ofBaseApp::mouseReleased));
    ofAddListener(dashboardWindow->events().mouseScrolled, mainApp.get(), static_cast<void (ofApp::*)(ofMouseEventArgs&)>(&ofBaseApp::mouseScrolled));

    // Démarrage de l'application sur la fenêtre de rendu principal
    ofRunApp(outputWindow, mainApp);
    ofRunMainLoop();
}
