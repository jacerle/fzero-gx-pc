#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PresentPush {
    float crt_enabled;
    float scanline_strength;
    float mask_strength;
    float curvature;
    float vignette;
    float brightness;
    float src_width;
    float src_height;
    float dst_width;
    float dst_height;
} u;

vec2 apply_curvature(vec2 uv, float k) {
    vec2 c = uv * 2.0 - 1.0;
    vec2 offset = abs(c.yx) / vec2(4.0, 4.0);
    c = c + c * offset * offset * (k * 8.0);
    return c * 0.5 + 0.5;
}

void main() {
    vec2 uv = v_uv;
    if (u.crt_enabled > 0.5 && u.curvature > 0.001) {
        uv = apply_curvature(uv, u.curvature);
    }

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 col = texture(u_tex, uv).rgb;

    if (u.crt_enabled > 0.5) {
        // Scanlines aligned to source vertical resolution (typically 480 lines when CRT feed is active)
        float lines = max(u.src_height, 240.0);
        float scan = 0.5 + 0.5 * cos(uv.y * lines * 6.2831853);
        float scan_factor = mix(1.0, 1.0 - u.scanline_strength * 0.75, scan);
        col *= scan_factor;

        // Subtle RGB aperture-grille phosphor mask in destination screen pixels
        float px_x = gl_FragCoord.x;
        float sub = mod(px_x, 3.0);
        vec3 mask = vec3(
            sub < 1.0 ? 1.0 : 0.72,
            (sub >= 1.0 && sub < 2.0) ? 1.0 : 0.72,
            sub >= 2.0 ? 1.0 : 0.72
        );
        col *= mix(vec3(1.0), mask, u.mask_strength);

        // Edge vignette
        vec2 vig_uv = uv * (1.0 - uv);
        float vig = clamp(pow(vig_uv.x * vig_uv.y * 16.0, 0.25), 0.0, 1.0);
        col *= mix(1.0, vig, u.vignette);
        col *= u.brightness;
    }

    out_color = vec4(col, 1.0);
}
