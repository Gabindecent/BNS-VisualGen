#version 120

// MANDALA — kaléidoscope polaire + bras en spirale logarithmique + chair fractale.
// Symétrie d'ordre 6 à 18 selon la complexité ; les kicks font respirer les anneaux.

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
    vec2 uv = (gl_FragCoord.xy - 0.5 * u_resolution) / u_resolution.y;
    uv /= u_scale;
    float r = length(uv);
    float a = atan(uv.y, uv.x);
    float t = u_time * 0.3;

    // Repliement kaléidoscopique : un secteur, mis en miroir
    float segments = floor(6.0 + 12.0 * u_complexity);
    float sector = 6.2831853 / segments;
    a = abs(mod(a, sector) - 0.5 * sector);
    vec2 k = vec2(cos(a), sin(a)) * r;

    float breathe = 1.0 + 0.3 * u_pulse;
    float rings = r * (3.0 + 5.0 * u_complexity) / breathe;
    float spiral = 0.6 * sin(a * 4.0 + log(r + 0.02) * 2.5 - t * 1.5);
    float flesh = 0.5 * fbm(k * 3.0 + vec2(t * 0.2, -t * 0.15));
    float field = rings + spiral + flesh - t * 0.4 + u_mid * 0.15 * sin(r * 20.0 - t * 4.0);
    gl_FragColor = vec4(field, 0.0, 0.0, 1.0);
}
