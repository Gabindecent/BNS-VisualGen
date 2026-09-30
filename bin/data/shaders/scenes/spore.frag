#version 120

// SPORE — spores qui dérivent (cellules de Voronoï sur un domaine déformé par du bruit),
// cernes de croissance autour de chacune, et filaments de mycélium qui les relient.

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

vec2 hash2(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453);
}

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * u_resolution) / u_resolution.y;
    float t = u_time * 0.25;
    vec2 p = uv * (3.0 + 5.0 * u_complexity) / u_scale;
    p += 0.35 * vec2(fbm(p * 0.8 + t * 0.3), fbm(p * 0.8 - t * 0.27 + 4.0)); // formes molles

    vec2 cell = floor(p);
    vec2 local = fract(p);
    float d1 = 8.0;
    float d2 = 8.0;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 g = vec2(float(i), float(j));
            vec2 o = hash2(cell + g);
            o = 0.5 + 0.4 * sin(t + 6.2831853 * o); // chaque spore dérive sur son orbite
            vec2 d = g + o - local;
            float dist = dot(d, d);
            if (dist < d1) {
                d2 = d1;
                d1 = dist;
            } else if (dist < d2) {
                d2 = dist;
            }
        }
    }
    d1 = sqrt(d1);
    d2 = sqrt(d2);

    float growth = d1 * (3.0 + 2.0 * u_kick) - t * 0.8;             // cernes de croissance
    float mycelium = (d2 - d1) < (0.06 + 0.06 * u_mid) ? 0.5 : 0.0; // filaments = demi-cycle décalé
    gl_FragColor = vec4(growth + mycelium + u_pulse * 0.1, 0.0, 0.0, 1.0);
}
