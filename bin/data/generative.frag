#version 120

varying vec2 texCoordVarying;

uniform float u_time;
uniform float u_aspect;
uniform vec2 u_resolution;
uniform float u_lineDensity;
uniform float u_warpAmount;
uniform float u_swirlAmount;
uniform float u_ringAmount;

uniform vec3 u_colorLight;
uniform vec3 u_colorMid;
uniform vec3 u_colorDark;

// Réactivité audio (0 si Audio Reactive est désactivé)
uniform float u_bass;
uniform float u_mid;
uniform float u_treble;

void main()
{
    // On utilise gl_FragCoord (pixels écran) plutôt que texCoordVarying : ofDrawRectangle()
    // ne garantit pas des coordonnées de texture normalisées 0..1, ce qui provoquait des
    // valeurs aberrantes (écran blanc / NaN) dans les fonctions trigonométriques ci-dessous.
    vec2 uv = (gl_FragCoord.xy / u_resolution) * 2.0 - 1.0;
    uv.x *= u_aspect;

    float r = length(uv);
    float ang = atan(uv.y, uv.x);

    // --- Vortex central : tord l'angle d'autant plus fort qu'on est proche du centre ---
    float swirl = (u_swirlAmount + u_bass * 3.0) / (r * 3.0 + 0.35);
    ang += swirl + u_time * 0.15;
    vec2 p = vec2(cos(ang), sin(ang)) * r;

    // --- Champ de lignes moiré : ondulation sinusoïdale le long d'un axe ---
    float warp = sin(p.x * 3.0 + u_time * 0.6) * (u_warpAmount + u_mid * 2.0);
    float linesPhase = (p.y + warp) * u_lineDensity;
    float lineWave = abs(fract(linesPhase) - 0.5) * 2.0; // triangle 0..1 : pointes de noir aux jonctions

    // --- Anneaux concentriques (esthétique demi-teinte / cible) ---
    float ringPhase = r * 10.0 - u_time * 0.4;
    float ring = abs(fract(ringPhase) - 0.5) * 2.0;

    float pattern = mix(lineWave, ring, clamp(u_ringAmount, 0.0, 1.0));
    // Les aigus accentuent le contraste du motif (pics plus nets)
    pattern = pow(pattern, 1.0 + u_treble * 1.5);

    // --- Tricolore : dégradé clair/accent selon la phase, assombri vers le noir aux creux ---
    vec3 stripeColor = mix(u_colorLight, u_colorMid, sin(p.x * 1.3 + p.y * 1.7 + u_time * 0.3) * 0.5 + 0.5);
    vec3 col = mix(u_colorDark, stripeColor, pattern);

    gl_FragColor = vec4(col, 1.0);
}
