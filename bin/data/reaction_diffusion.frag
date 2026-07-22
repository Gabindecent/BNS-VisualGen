#version 120

// Un pas de simulation du modèle de Turing (réaction-diffusion de Gray-Scott) :
// deux "produits chimiques" U et V (stockés dans les canaux R et G) qui diffusent et
// réagissent entre eux -> motifs organiques (taches, rayures, corail) retrouvés dans la nature
// (pelage animal, coquillages...), ici utilisés comme troisième source de motifs psychédéliques.

uniform sampler2DRect tex0; // état précédent : R = U, G = V
uniform float feed;  // taux d'apport de U ("Feed")
uniform float kill;  // taux de disparition de V ("Kill")
uniform float du;    // diffusion de U (fixe, ~1.0)
uniform float dv;    // diffusion de V (réglable)

varying vec2 texCoordVarying;

void main()
{
    vec2 uv = texture2DRect(tex0, texCoordVarying).rg;

    // Laplacien discret (noyau pondéré classique du Gray-Scott : centre -1, voisins directs 0.2,
    // diagonales 0.05) -> approxime la diffusion des deux produits chimiques
    vec2 lap = uv * -1.0;
    lap += 0.2 * (
        texture2DRect(tex0, texCoordVarying + vec2( 1.0,  0.0)).rg +
        texture2DRect(tex0, texCoordVarying + vec2(-1.0,  0.0)).rg +
        texture2DRect(tex0, texCoordVarying + vec2( 0.0,  1.0)).rg +
        texture2DRect(tex0, texCoordVarying + vec2( 0.0, -1.0)).rg
    );
    lap += 0.05 * (
        texture2DRect(tex0, texCoordVarying + vec2( 1.0,  1.0)).rg +
        texture2DRect(tex0, texCoordVarying + vec2( 1.0, -1.0)).rg +
        texture2DRect(tex0, texCoordVarying + vec2(-1.0,  1.0)).rg +
        texture2DRect(tex0, texCoordVarying + vec2(-1.0, -1.0)).rg
    );

    float u = uv.x;
    float v = uv.y;
    float reaction = u * v * v;

    float newU = u + (du * lap.x - reaction + feed * (1.0 - u));
    float newV = v + (dv * lap.y + reaction - (feed + kill) * v);

    gl_FragColor = vec4(clamp(newU, 0.0, 1.0), clamp(newV, 0.0, 1.0), 0.0, 1.0);
}
