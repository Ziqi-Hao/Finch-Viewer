#version 430
// Programmable vertex pulling: no vertex buffer. The deformed positions/normals come
// from the compute pass's SSBOs, indexed by (instance = voxel, vertex = icosphere
// vertex id). The draw call is an instanced drawIndexed over the shared icosphere
// index buffer: gl_InstanceIndex selects the glyph, gl_VertexIndex the sphere vertex.
layout(std430, binding = 1) readonly buffer Pos  { vec4 positions[]; };  // [nVox*nDir]
layout(std430, binding = 2) readonly buffer Nor  { vec4 normals[];   };  // [nVox*nDir]
layout(std430, binding = 3) readonly buffer Dirs { vec4 sphereDir[]; };  // [nDir]

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    vec4 lightDir;
    int nDir;
} ubo;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;

void main() {
    uint gi = uint(gl_InstanceIndex) * uint(ubo.nDir) + uint(gl_VertexIndex);
    vNormal = normals[gi].xyz;
    vColor = abs(normalize(sphereDir[gl_VertexIndex].xyz));  // DEC color from base direction
    gl_Position = ubo.mvp * vec4(positions[gi].xyz, 1.0);
}
