#version 120

// Mode Génératif "nébuleuse" : nappes de couleur organiques qui dérivent lentement, sans forme
// géométrique reconnaissable (contrairement à l'ancienne version moiré/anneaux, plus nerveuse).
// Technique : bruit fractal (fbm) + "domain warping" (Inigo Quilez) -> chaque étage de bruit
// déforme l'espace d'entrée de l'étage suivant, ce qui donne des volutes qui ne se répètent
// jamais et qui semblent couler plutôt que tourner/clignoter.

varying vec2 texCoordVarying;

uniform float u_time;
uniform float u_aspect;
uniform vec2 u_resolution;
uniform float u_nebulaScale;    // zoom du champ de bruit -> taille des volutes
uniform float u_nebulaWarp;     // intensité de la déformation -> à quel point ça "coule"
uniform float u_nebulaContrast; // netteté des transitions de couleur

uniform vec3 u_colorLight;
uniform vec3 u_colorMid;
uniform vec3 u_colorDark;

// Réactivité audio (0 si Audio Reactive est désactivé)
uniform float u_bass;
uniform float u_mid;
uniform float u_treble;

// Bruit de valeur 2D (GLSL 1.20 n'a pas de bruit natif) + fbm (somme d'octaves à amplitude
// décroissante) : la brique de base de tous les effets "organiques" (nuages, fumée, lave).
float hash(vec2 p){
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p){
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f); // lissage (Hermite) : évite les facettes visibles
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

float fbm(vec2 p){
    float value = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++) {
        value += amp * noise(p);
        p *= 2.02;
        amp *= 0.5;
    }
    return value;
}

void main()
{
    // gl_FragCoord (pixels écran) plutôt que texCoordVarying : ofDrawRectangle() ne garantit pas
    // des coordonnées de texture normalisées, ce qui donnait des valeurs aberrantes ailleurs.
    vec2 uv = (gl_FragCoord.xy / u_resolution) * 2.0 - 1.0;
    uv.x *= u_aspect;

    float t = u_time; // déjà mis à l'échelle côté C++ (slider "Vitesse Derive")
    vec2 p = uv * u_nebulaScale;

    // Domain warping en 3 étages : chaque champ de bruit (q, r) sert à décaler l'entrée du
    // suivant -> les volutes se tordent et dérivent sans jamais se répéter à l'identique.
    float warp = u_nebulaWarp + u_bass * 2.5; // les basses gonflent temporairement les volutes
    vec2 q = vec2(
        fbm(p + t * 0.05),
        fbm(p + vec2(5.2, 1.3) - t * 0.04)
    );
    vec2 r = vec2(
        fbm(p + warp * q + vec2(1.7, 9.2) + t * 0.08),
        fbm(p + warp * q + vec2(8.3, 2.8) + t * 0.06)
    );
    float f = fbm(p + warp * r);

    // Les aigus ajoutent un grain fin (détail haute fréquence) sans casser le côté lent global
    f += u_treble * 0.08 * (noise(p * 6.0 + t * 0.3) - 0.5);

    // --- Colorisation : dégradé continu clair/accent/creux, aucune bande dure ---
    float tone = clamp(pow(max(f, 0.0), u_nebulaContrast), 0.0, 1.0);
    vec3 col = mix(u_colorDark, u_colorMid, smoothstep(0.15, 0.55, tone));
    col = mix(col, u_colorLight, smoothstep(0.5, 0.95, tone));

    // Très légère respiration de teinte accent, au rythme du champ de warp (donne un soupçon de
    // "mouvement de couleur" en plus du mouvement de forme, sans devenir criard)
    float driftMix = sin(length(q) * 2.0 + length(r) * 1.5 + t * 0.1) * 0.5 + 0.5;
    col = mix(col, u_colorMid, driftMix * 0.12);

    gl_FragColor = vec4(col, 1.0);
}
