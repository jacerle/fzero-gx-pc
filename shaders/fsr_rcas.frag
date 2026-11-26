#version 450

// AMD FidelityFX Super Resolution 1.0 - RCAS (Robust Contrast Adaptive Sharpening)
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform FsrPush {
    float src_width;
    float src_height;
    float dst_width;
    float dst_height;
    float sharpness; // 0.0 (soft) .. 1.0 (maximum RCAS sharpening)
    float pad0;
    float pad1;
    float pad2;
} u;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec2 dst_sz = max(vec2(u.dst_width, u.dst_height), vec2(1.0));
    vec2 inv = 1.0 / dst_sz;

    vec3 b = texture(u_tex, v_uv + vec2(0.0, -inv.y)).rgb;
    vec3 d = texture(u_tex, v_uv + vec2(-inv.x, 0.0)).rgb;
    vec3 e = texture(u_tex, v_uv).rgb;
    vec3 f = texture(u_tex, v_uv + vec2(inv.x, 0.0)).rgb;
    vec3 h = texture(u_tex, v_uv + vec2(0.0, inv.y)).rgb;

    float bL = luma(b);
    float dL = luma(d);
    float eL = luma(e);
    float fL = luma(f);
    float hL = luma(h);

    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float mx = max(max(max(bL, dL), max(fL, hL)), eL);
    float mn = min(min(min(bL, dL), min(fL, hL)), eL);
    float range = max(mx - mn, 1e-4);
    nz = clamp(abs(nz) / range, 0.0, 1.0);
    nz = -0.5 * nz + 1.0;

    vec3 mnC = min(min(b, d), min(f, h));
    vec3 mxC = max(max(b, d), max(f, h));

    // RCAS weight limit: 0.25 * sharpness_gain
    float stops = mix(2.0, 0.0, clamp(u.sharpness, 0.0, 1.0));
    float peak = 0.25 * exp2(-stops);
    vec3 hitMin = mnC / max(4.0 * mxC, vec3(1e-4));
    vec3 hitMax = (1.0 - mxC) / max(4.0 * mnC - 4.0, vec3(-1e-4));
    vec3 lobe = max(-hitMin, hitMax);
    float w = max(-peak, min(max(max(lobe.r, lobe.g), lobe.b), 0.0)) * nz;

    vec3 col = clamp((b * w + d * w + f * w + h * w + e) / (4.0 * w + 1.0), vec3(0.0), vec3(1.0));
    out_color = vec4(col, 1.0);
}
