#version 430
// FA background slice: a quad in VOXEL coordinates, placed into world (RAS mm) by
// the voxel->world affine, with a 3D-texture coordinate at voxel centres. Mirrors
// the OpenGL editor's kSliceVertexShader.
layout(location = 0) in vec3 inVoxel;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    mat4 voxToWorld;
    vec4 invDims;      // xyz = 1/dims
    vec4 valueParams;  // x = valueMin, y = valueRange
} ubo;

layout(location = 0) out vec3 vTex;

void main() {
    vTex = (inVoxel + vec3(0.5)) * ubo.invDims.xyz;     // sample voxel centres
    gl_Position = ubo.mvp * (ubo.voxToWorld * vec4(inVoxel, 1.0));
}
