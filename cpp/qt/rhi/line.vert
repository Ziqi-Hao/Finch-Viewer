#version 430
// Streamline / cage / box line vertex shader for the RHI viewport. Interleaved
// [pos.xyz, color.rgb] vertices (same layout BuildDisplayLineGeometry emits).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(std140, binding = 0) uniform UBO { mat4 mvp; } ubo;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = inColor;
    gl_Position = ubo.mvp * vec4(inPos, 1.0);
}
