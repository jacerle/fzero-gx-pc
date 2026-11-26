#version 450

// AMD FidelityFX Super Resolution 1.0 - EASU (Edge Adaptive Spatial Upsampling)
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform FsrPush {
    float src_width;
    float src_height;
    float dst_width;
    float dst_height;
    float sharpness;
    float pad0;
    float pad1;
    float pad2;
} u;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void easu_set(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
              float lA, float lB, float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 1e-5 ? (1.0 / lenX) : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 1e-5 ? (1.0 / lenY) : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void main() {
    vec2 src_sz = max(vec2(u.src_width, u.src_height), vec2(1.0));
    vec2 inv_src = 1.0 / src_sz;
    vec2 pos = v_uv * src_sz - 0.5;
    vec2 fp = floor(pos);
    vec2 pp = pos - fp;

    // 12-tap neighborhood in source texels:
    //     b c
    //   e f g h
    //   i j k l
    //     n o
    vec3 b = texture(u_tex, (fp + vec2( 0.5, -0.5)) * inv_src).rgb;
    vec3 c = texture(u_tex, (fp + vec2( 1.5, -0.5)) * inv_src).rgb;
    vec3 e = texture(u_tex, (fp + vec2(-0.5,  0.5)) * inv_src).rgb;
    vec3 f = texture(u_tex, (fp + vec2( 0.5,  0.5)) * inv_src).rgb;
    vec3 g = texture(u_tex, (fp + vec2( 1.5,  0.5)) * inv_src).rgb;
    vec3 h = texture(u_tex, (fp + vec2( 2.5,  0.5)) * inv_src).rgb;
    vec3 i = texture(u_tex, (fp + vec2(-0.5,  1.5)) * inv_src).rgb;
    vec3 j = texture(u_tex, (fp + vec2( 0.5,  1.5)) * inv_src).rgb;
    vec3 k = texture(u_tex, (fp + vec2( 1.5,  1.5)) * inv_src).rgb;
    vec3 l = texture(u_tex, (fp + vec2( 2.5,  1.5)) * inv_src).rgb;
    vec3 n = texture(u_tex, (fp + vec2( 0.5,  2.5)) * inv_src).rgb;
    vec3 o = texture(u_tex, (fp + vec2( 1.5,  2.5)) * inv_src).rgb;

    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f);
    float gL = luma(g), hL = luma(h), iL = luma(i), jL = luma(j);
    float kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    easu_set(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    easu_set(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    easu_set(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    easu_set(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

    float dirR = dot(dir, dir);
    if (dirR < 1e-6) {
        dir = vec2(1.0, 0.0);
    } else {
        dir *= inversesqrt(dirR);
    }

    len = len * 0.5;
    len *= len;
    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + (0.21 - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easu_tap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easu_tap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
    easu_tap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);
    easu_tap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);

    vec3 col = clamp(aC / max(aW, 1e-5), min4, max4);
    out_color = vec4(col, 1.0);
}
