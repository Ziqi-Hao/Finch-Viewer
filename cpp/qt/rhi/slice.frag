#version 430
// Grayscale + alpha ramp over the FA value, identical to the OpenGL editor's
// kSliceFragmentShader: a = t<0.025 ? 0 : clamp(0.12 + 0.58*t).
layout(location = 0) in vec3 vTex;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    mat4 voxToWorld;
    vec4 invDims;
    vec4 valueParams;  // x = valueMin, y = valueRange
} ubo;
layout(binding = 1) uniform sampler3D uVol;

layout(location = 0) out vec4 outColor;

void main() {
    float t = clamp((texture(uVol, vTex).r - ubo.valueParams.x) / ubo.valueParams.y, 0.0, 1.0);
    float a = t < 0.025 ? 0.0 : clamp(0.12 + 0.58 * t, 0.0, 1.0);
    if (a <= 0.0) discard;
    outColor = vec4(vec3(t), a);
}
