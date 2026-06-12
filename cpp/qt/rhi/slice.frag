#version 430
// Background image slice. Two modes (valueParams.w): grayscale window (volumes)
// or integer-label colour lookup via uLut (segmentations / atlases). Layer
// opacity (valueParams.z) scales the output alpha for FSLeyes-style blending.
layout(location = 0) in vec3 vTex;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    mat4 voxToWorld;
    vec4 invDims;      // xyz = 1/dims, w = label LUT width (label mode)
    vec4 valueParams;  // x = valueMin, y = valueRange, z = opacity, w = mode (0 gray, 1 label)
} ubo;
layout(binding = 1) uniform sampler3D uVol;
layout(binding = 2) uniform sampler2D uLut;  // per-label RGBA colour table (label mode)

layout(location = 0) out vec4 outColor;

void main() {
    float raw = texture(uVol, vTex).r;
    if (ubo.valueParams.w > 1.5) {              // heatmap mode (track density)
        float t = clamp((raw - ubo.valueParams.x) / ubo.valueParams.y, 0.0, 1.0);
        if (t < 0.03) discard;                  // sparse -> transparent
        // "hot" colour ramp: black -> red -> yellow -> white.
        vec3 c = clamp(vec3(3.0 * t, 3.0 * t - 1.0, 3.0 * t - 2.0), 0.0, 1.0);
        outColor = vec4(c, clamp(0.25 + 0.75 * t, 0.0, 1.0) * ubo.valueParams.z);
    } else if (ubo.valueParams.w > 0.5) {       // label mode (uVol sampled nearest)
        int idx = int(raw + 0.5);
        if (idx <= 0) discard;                  // 0 = background
        int w = max(int(ubo.invDims.w), 1);
        idx = clamp(idx, 0, w - 1);
        vec4 c = texelFetch(uLut, ivec2(idx, 0), 0);
        if (c.a <= 0.0) discard;
        outColor = vec4(c.rgb, c.a * ubo.valueParams.z);
    } else {                                    // grayscale window
        float t = clamp((raw - ubo.valueParams.x) / ubo.valueParams.y, 0.0, 1.0);
        float a = t < 0.025 ? 0.0 : clamp(0.12 + 0.58 * t, 0.0, 1.0);
        a *= ubo.valueParams.z;
        if (a <= 0.0) discard;
        outColor = vec4(vec3(t), a);
    }
}
