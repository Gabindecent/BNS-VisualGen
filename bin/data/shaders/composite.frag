#version 120
#extension GL_ARB_texture_rectangle : enable

// Passe composite : l'UNIQUE endroit du pipeline où naît la couleur.
//
// Entrées : champ scalaire de la scène active (0..1), champ de la scène sortante (transition),
// champ de la couche webcam. Sortie : aplats de couleurs pleines de la palette.
//
// Règles de la direction artistique, garanties ici pour toutes les scènes :
//   - aucun dégradé : le champ est découpé en bandes, chaque bande = UNE couleur de palette,
//     jamais d'interpolation entre deux couleurs ;
//   - aucun contour noir : seules des couleurs de palette sortent (le C++ refuse toute couleur
//     sombre ou terne), le fond lui-même est un aplat.

uniform sampler2DRect u_field;
uniform sampler2DRect u_prevField;
uniform sampler2DRect u_camField;
uniform vec2 u_fieldSize;   // taille des champs (pixels)
uniform vec2 u_outSize;     // taille de la sortie (pixels)

uniform float u_mix;        // transition : 0 = scène sortante, 1 = scène active
uniform float u_block;      // taille des blocs de la trame de transition (pixels de sortie)

uniform vec3 u_palette[8];
uniform int u_paletteSize;
uniform float u_bands;      // nombre d'aplats découpés dans le champ
uniform float u_bandOffset; // défilement des aplats (0..1)

uniform int u_camMode;      // 0 = off, 1 = caméra seule, 2 = silhouette, 3 = ajout
uniform float u_camAmount;
uniform float u_camThreshold;

// Trame ordonnée de Bayer 4x4 (valeurs k/16) : la transition fait basculer des BLOCS entiers
// d'une scène à l'autre, sans jamais mélanger deux couleurs (un fondu serait un dégradé).
float bayer2(vec2 a) {
    a = floor(a);
    return fract(a.x * 0.5 + a.y * a.y * 0.75);
}
float bayer4(vec2 a) {
    return bayer2(0.5 * a) * 0.25 + bayer2(a);
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_outSize;
    vec2 fc = uv * u_fieldSize; // champs à résolution réduite, lus avec filtrage linéaire

    float v = texture2DRect(u_field, fc).r;
    if (u_mix < 1.0 && bayer4(floor(gl_FragCoord.xy / u_block)) >= u_mix) {
        v = texture2DRect(u_prevField, fc).r;
    }

    if (u_camMode != 0) {
        float cam = texture2DRect(u_camField, fc).r;
        if (u_camMode == 1) {
            v = cam * (1.0 + 3.0 * u_camAmount);                       // caméra seule
        } else if (u_camMode == 2) {
            if (cam > u_camThreshold) {                                // silhouette : décale les
                v += max(0.5 * u_camAmount, 1.0 / u_bands);            // aplats d'au moins 1 bande
            }
        } else {
            v += cam * u_camAmount * 2.0;                              // ajout : le motif se tord
        }
    }

    // Quantification : bande entière -> index de palette -> couleur pleine
    float band = floor(fract(v + u_bandOffset) * u_bands);
    float index = mod(band, float(u_paletteSize));
    vec3 color = u_palette[0];
    for (int i = 1; i < 8; i++) {
        if (abs(float(i) - index) < 0.5) color = u_palette[i];
    }
    gl_FragColor = vec4(color, 1.0);
}
