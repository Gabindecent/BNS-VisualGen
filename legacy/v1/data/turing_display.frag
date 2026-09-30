#version 120

// Colorise le résultat de la simulation de Turing (canal V = concentration du second produit)
// avec la palette tricolore de l'onglet, comme les autres motifs génératifs.

uniform sampler2DRect tex0; // R = U, G = V
uniform vec3 u_colorLight;
uniform vec3 u_colorMid;
uniform vec3 u_colorDark;

// Masquage optionnel par la luminosité de l'image déjà composée (webcam + génératif) en dessous :
// 0 = plein écran, 1 = uniquement les zones claires, 2 = uniquement les zones sombres.
uniform sampler2DRect u_maskTex;
uniform vec2 u_maskSize; // taille (pixels) de u_maskTex
uniform vec2 u_rdSize;   // taille (pixels) de tex0 (résolution réduite de la simulation)
uniform int u_maskMode;
uniform float u_maskThreshold;

varying vec2 texCoordVarying;

void main()
{
    float v = texture2DRect(tex0, texCoordVarying).g;

    // v évolue typiquement dans une plage étroite (~0 à 0.35) : on amplifie pour un bon contraste
    float t = clamp(v * 3.2, 0.0, 1.0);
    vec3 col = mix(u_colorDark, mix(u_colorMid, u_colorLight, t), t);

    float mask = 1.0;
    if (u_maskMode != 0) {
        // texCoordVarying est dans l'espace de tex0 (rdWidth x rdHeight) : on normalise puis on
        // remet à l'échelle de u_maskTex pour échantillonner le même point de l'image.
        vec2 uv = texCoordVarying / u_rdSize;
        vec3 srcColor = texture2DRect(u_maskTex, uv * u_maskSize).rgb;
        float lum = dot(srcColor, vec3(0.299, 0.587, 0.114));
        float edge = 0.12;
        if (u_maskMode == 1) {
            mask = smoothstep(u_maskThreshold - edge, u_maskThreshold + edge, lum);
        } else {
            mask = smoothstep(u_maskThreshold + edge, u_maskThreshold - edge, lum);
        }
    }

    gl_FragColor = vec4(col, mask);
}
