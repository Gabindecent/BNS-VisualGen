#version 120
#extension GL_ARB_texture_rectangle : enable

// Morphogen — conversion de la simulation Gray-Scott en champ scalaire.
// La concentration de V (canal G) dessine les motifs (taches, labyrinthes, mycélium) ; elle
// évolue dans une plage étroite (~0..0.35), d'où l'amplification.

uniform sampler2DRect tex0; // état de la simulation : R = U, G = V
uniform float u_pulse;      // impulsion de kick : les aplats "sautent" d'un cran
varying vec2 texCoordVarying;

void main() {
    float v = texture2DRect(tex0, texCoordVarying).g;
    float field = clamp(v * 3.2, 0.0, 1.0) + u_pulse * 0.08;
    gl_FragColor = vec4(field, 0.0, 0.0, 1.0);
}
