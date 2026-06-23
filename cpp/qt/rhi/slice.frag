#version 430
// Background image slice. Modes (valueParams.w): 0 grayscale window (volumes),
// 1 integer-label colour lookup via uLut (segmentations / atlases), 2 "hot"
// heatmap (track density), 3 signed statistical overlay (fMRI z/t/r maps:
// diverging hot/cool colour + hard threshold). Layer opacity (valueParams.z)
// scales the output alpha for FSLeyes-style blending.
layout(location = 0) in vec3 vTex;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
    mat4 voxToWorld;
    vec4 invDims;      // xyz = 1/dims, w = label LUT width (label mode)
    // z = opacity; w = mode. x/y reinterpreted per mode:
    //   gray/heatmap: x = valueMin/threshold, y = range
    //   stat (w=3):   x = |stat| threshold,   y = cap - threshold
    vec4 valueParams;
} ubo;
layout(binding = 1) uniform sampler3D uVol;
layout(binding = 2) uniform sampler2D uLut;  // per-label RGBA colour table (label mode)

layout(location = 0) out vec4 outColor;

void main() {
    float raw = texture(uVol, vTex).r;
    if (ubo.valueParams.w > 2.5) {              // stat overlay (signed z/t/r): diverging + hard threshold
        // |stat| below the display threshold is fully transparent (discard) so the
        // anatomy underneath shows through — a hard edge, not the soft grayscale ramp.
        float a = abs(raw);
        if (a < ubo.valueParams.x) discard;
        float t = clamp((a - ubo.valueParams.x) / ubo.valueParams.y, 0.0, 1.0);
        // FSLeyes convention: positive -> red->yellow ("hot"), negative -> blue->cyan ("cool").
        vec3 c = raw >= 0.0 ? vec3(1.0, t, 0.0) : vec3(0.0, t, 1.0);
        outColor = vec4(c, ubo.valueParams.z);  // constant per-layer opacity above threshold
    } else if (ubo.valueParams.w > 1.5) {       // heatmap mode (track density)
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
