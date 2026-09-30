#version 120

// FLUID — nappes fractales qui coulent (domain warping à 3 étages, d'après I. Quilez).
// Sortie : champ scalaire (canal R). Les mix() ci-dessous mélangent des VALEURS de bruit,
// jamais des couleurs : la palette découpera ce champ en aplats nets.

uniform float u_time;       // horloge propre à la scène (vitesse et audio déjà intégrés)
uniform vec2 u_resolution;  // taille du champ (pixels)
uniform float u_kick;       // 0..1 lissé
uniform float u_mid;
uniform float u_high;
uniform float u_pulse;      // impulsion de kick (1 -> 0)
uniform float u_speed;
uniform float u_scale;
uniform float u_complexity; // 0..1
uniform float u_audio;

// Bruit de valeur 2D + fbm : la brique des formes organiques (GLSL 1.20 n'a pas de bruit natif)
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p) {
    float value = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++) {
        value += amp * noise(p);
        p = p * 2.02 + vec2(1.7, 9.2);
        amp *= 0.5;
    }
    return value;
}

void main() {
    vec2 uv = (gl_FragCoord.xy / u_resolution) * 2.0 - 1.0;
    uv.x *= u_resolution.x / u_resolution.y;
    vec2 p = uv * 1.2 / u_scale;
    float t = u_time * 0.15;

    float warp = 1.5 + 3.0 * u_complexity + 1.5 * u_kick; // les basses gonflent les volutes
    vec2 q = vec2(fbm(p + t * 0.30), fbm(p + vec2(5.2, 1.3) - t * 0.25));
    vec2 r = vec2(fbm(p + warp * q + vec2(1.7, 9.2) + t * 0.40),
                  fbm(p + warp * q + vec2(8.3, 2.8) + t * 0.35));
    float f = fbm(p + warp * r);
    f += u_high * 0.05 * (noise(p * 9.0 + t * 3.0) - 0.5); // grain fin sur les aigus

    // Plus de complexité = plus d'aplats traversés = motif plus dense
    float field = f * (1.0 + 2.5 * u_complexity) + length(r) * 0.3 + u_pulse * 0.1;
    gl_FragColor = vec4(field, 0.0, 0.0, 1.0);
}
