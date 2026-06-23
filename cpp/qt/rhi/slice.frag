#version 430
// Background image slice. Modes (valueParams.w): 0 grayscale window (volumes),
// 1 integer-label colour lookup via uLut (segmentations / atlases), 2 "hot"
// heatmap (track density), 3 signed statistical overlay (fMRI z/t/r maps:
// diverging hot/cool colour + hard threshold), 4 viridis (perceptually-uniform
// sequential, for positive continuous metrics — ReHo/ALFF/…). Layer opacity
// (valueParams.z) scales the output alpha for FSLeyes-style blending.
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

// Viridis (matplotlib) as a degree-6 polynomial — the standard analytic fit
// (Matt Zucker); visually faithful (endpoints match the true colormap to <2/255)
// so no LUT/sampler binding is needed. Perceptually uniform + colour-blind safe.
vec3 viridis(float t) {
    const vec3 c0 = vec3(0.2777273272234177, 0.005407344544966578, 0.3340998053353061);
    const vec3 c1 = vec3(0.1050930431085774, 1.404613529898575, 1.384590162594685);
    const vec3 c2 = vec3(-0.3308618287255563, 0.214847559468213, 0.09509516302823659);
    const vec3 c3 = vec3(-4.634230498983486, -5.799100973351585, -19.33244095627987);
    const vec3 c4 = vec3(6.228269936347081, 14.17993336680509, 56.69055260068105);
    const vec3 c5 = vec3(4.776384997670288, -13.74514537774601, -65.35303263337234);
    const vec3 c6 = vec3(-5.435455855934631, 4.645852612178535, 26.3124352495832);
    return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

void main() {
    float raw = texture(uVol, vTex).r;
    if (ubo.valueParams.w > 3.5) {              // viridis (perceptually-uniform positive scalar)
        float t = clamp((raw - ubo.valueParams.x) / ubo.valueParams.y, 0.0, 1.0);
        if (t < 0.02) discard;                  // near-min background -> transparent (anatomy shows)
        outColor = vec4(viridis(t), ubo.valueParams.z);
    } else if (ubo.valueParams.w > 2.5) {       // stat overlay (signed z/t/r): diverging + hard threshold
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
