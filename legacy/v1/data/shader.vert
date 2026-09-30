#version 120

varying vec2 texCoordVarying;

void main()
{
    // Récupérer les coordonnées de texture de base d'openFrameworks
    texCoordVarying = gl_MultiTexCoord0.xy;
    
    // Transformer la position spatiale (standard OpenGL 2)
    gl_Position = ftransform();
}
