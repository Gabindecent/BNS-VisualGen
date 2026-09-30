#version 120

// OF utilise des textures rectangulaires par défaut pour ofVideoGrabber
uniform sampler2DRect tex0; 

uniform vec4 u_color;      // Couleur fluo/punchy (zones claires) choisie dans le GUI
uniform vec4 u_darkColor;  // Couleur des zones sombres (remplace le noir pur, réglable dans le GUI)
uniform float u_threshold; // Limite de séparation clair/sombre (0.0 à 1.0)

varying vec2 texCoordVarying;

void main()
{
    // On échantillonne le pixel de la webcam
    vec4 texel = texture2DRect(tex0, texCoordVarying);

    // Calcul précis de la luminance (conversion en Niveaux de Gris)
    float luma = dot(texel.rgb, vec3(0.299, 0.587, 0.114));

    // Effet Bichromie "Stencil Tekno"
    // Les deux branches utilisent la MEME transparence (u_color.a) : avec un mélange alpha
    // (et non additif) dans la boucle de feedback, ça garantit un résultat toujours borné
    // entre 0 et 1, donc plus de saturation qui reste bloquée au blanc au bout de quelques minutes.
    if (luma > u_threshold) {
        // Pixel clair -> Couleur Punchy
        gl_FragColor = u_color;
    } else {
        // Pixel sombre -> Couleur sombre réglable (au lieu du noir fixe)
        gl_FragColor = vec4(u_darkColor.rgb, u_color.a);
    }
}
