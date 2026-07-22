#version 120

// Colorise le résultat de la simulation de Turing (canal V = concentration du second produit)
// avec la palette tricolore de l'onglet, comme les autres motifs génératifs.

uniform sampler2DRect tex0; // R = U, G = V
uniform vec3 u_colorLight;
uniform vec3 u_colorMid;
uniform vec3 u_colorDark;

varying vec2 texCoordVarying;

void main()
{
    float v = texture2DRect(tex0, texCoordVarying).g;

    // v évolue typiquement dans une plage étroite (~0 à 0.35) : on amplifie pour un bon contraste
    float t = clamp(v * 3.2, 0.0, 1.0);
    vec3 col = mix(u_colorDark, mix(u_colorMid, u_colorLight, t), t);

    gl_FragColor = vec4(col, 1.0);
}
