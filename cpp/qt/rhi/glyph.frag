#version 430
// Two-sided directional shading over the glyph's direction-encoded color. Two-sided
// (abs of N·L) because back-face culling is off (the inner faces of the ODF lobes are
// visible and should still be lit, not black). Mirrors cpp/odf/rhi_probe/glyph.frag.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    vec4 lightDir;
} ubo;

layout(location = 0) out vec4 fragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(ubo.lightDir.xyz);
    float diff = abs(dot(N, L));      // two-sided
    float amb = 0.35;
    vec3 c = vColor * (amb + 0.75 * diff);
    fragColor = vec4(c, 1.0);
}
