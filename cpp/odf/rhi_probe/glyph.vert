#version 430
// ODF glyph vertex shader. Positions arrive already in world (RAS mm) space from the
// CPU oracle BuildGlyphs (this milestone renders the verified CPU mesh; deform-on-GPU
// is the next step), so model == identity and we only apply the camera mvp.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    vec4 lightDir;  // world-space directional light (xyz)
} ubo;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;

void main() {
    gl_Position = ubo.mvp * vec4(inPos, 1.0);
    vNormal = inNormal;
    vColor = inColor;
}
