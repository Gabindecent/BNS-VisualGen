#version 120
#extension GL_ARB_texture_rectangle : enable

// Couche webcam : boucle de feedback ("Larsen vidéo") sur la LUMINANCE de la caméra.
// Sortie = un champ scalaire (canal R), colorisé ensuite par la passe composite comme le reste.
//
// À chaque frame : l'état précédent est relu zoomé + tourné autour du centre (effet tunnel),
// puis on y fond l'image caméra. Plus u_decay est proche de 1, plus les échos persistent.
// Point d'accroche des futurs effets caméra : déformer q (distorsions), ou traiter luma.

uniform sampler2DRect u_prev;   // état précédent de la boucle
uniform sampler2DRect u_cam;    // image caméra (texture rectangulaire d'ofVideoGrabber)
uniform vec2 u_size;            // taille du champ
uniform vec2 u_camSize;         // taille de l'image caméra
uniform float u_zoom;           // > 1 : les échos sont aspirés vers l'extérieur
uniform float u_angle;          // rotation par frame (radians)
uniform float u_decay;          // rémanence 0..1
uniform float u_gain;           // gain de luminance
uniform int u_mirror;           // 1 = miroir horizontal

void main() {
    vec2 uv = gl_FragCoord.xy / u_size;
    float aspect = u_size.x / u_size.y;

    // Relecture de l'état précédent, transformé autour du centre (repère non déformé)
    vec2 p = (uv - 0.5) * vec2(aspect, 1.0);
    float c = cos(u_angle);
    float s = sin(u_angle);
    p = vec2(c * p.x - s * p.y, s * p.x + c * p.y) / u_zoom;
    vec2 q = p / vec2(aspect, 1.0) + 0.5;
    float prev = 0.0;
    if (q.x >= 0.0 && q.x <= 1.0 && q.y >= 0.0 && q.y <= 1.0) {
        prev = texture2DRect(u_prev, q * u_size).r;
    }

    // Caméra recadrée pour remplir le champ sans déformation ; ligne 0 de l'image = haut
    vec2 cuv = vec2(u_mirror == 1 ? 1.0 - uv.x : uv.x, 1.0 - uv.y);
    float camAspect = u_camSize.x / u_camSize.y;
    if (aspect > camAspect) {
        cuv.y = 0.5 + (cuv.y - 0.5) * camAspect / aspect;
    } else {
        cuv.x = 0.5 + (cuv.x - 0.5) * aspect / camAspect;
    }
    vec3 rgb = texture2DRect(u_cam, cuv * u_camSize).rgb;
    float luma = clamp(dot(rgb, vec3(0.299, 0.587, 0.114)) * u_gain, 0.0, 1.0);

    // Mélange borné (jamais de saturation qui reste bloquée, même après des heures)
    gl_FragColor = vec4(mix(prev, luma, 1.0 - u_decay), 0.0, 0.0, 1.0);
}
