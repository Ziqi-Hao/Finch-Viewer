#version 430
// Handle points: like line.vert but also sets gl_PointSize (Metal takes point size
// from the vertex shader, not a pipeline state). Reuses line.frag for the fragment.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    vec4 params;   // params.x = point size (px)
} ubo;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = inColor;
    gl_Position = ubo.mvp * vec4(inPos, 1.0);
    gl_PointSize = ubo.params.x;
}
