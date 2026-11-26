#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform ScenePush {
    float time_sec;
    float aspect_ratio;
    float hud_edge_anchored;
    float rom_mounted;
    float stick_x;
    float stick_y;
    float trigger_l;
    float trigger_r;
    float res_x;
    float res_y;
} u;

float box_dist(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

vec3 smpte_bar(float x) {
    int idx = clamp(int(floor(x * 7.0)), 0, 6);
    if (idx == 0) return vec3(0.75, 0.75, 0.75);
    if (idx == 1) return vec3(0.75, 0.75, 0.00);
    if (idx == 2) return vec3(0.00, 0.75, 0.75);
    if (idx == 3) return vec3(0.00, 0.75, 0.00);
    if (idx == 4) return vec3(0.75, 0.00, 0.75);
    if (idx == 5) return vec3(0.75, 0.00, 0.00);
    return vec3(0.00, 0.00, 0.75);
}

void main() {
    // Hor+ projection: vertical FOV is fixed; horizontal view expands with aspect_ratio.
    vec2 ndc = v_uv * 2.0 - 1.0;
    vec2 p = vec2(ndc.x * u.aspect_ratio, -ndc.y);

    // Sky gradient + distant synth sun/city horizon
    float horizon = 0.08 + u.stick_y * 0.05;
    vec3 sky_top = vec3(0.02, 0.03, 0.10);
    vec3 sky_bot = vec3(0.10, 0.14, 0.34);
    vec3 col = mix(sky_bot, sky_top, clamp((p.y - horizon) * 1.15, 0.0, 1.0));

    // Sun glow above horizon
    vec2 sun_pos = vec2(-u.stick_x * 0.35, horizon + 0.24);
    float sun_d = length(p - sun_pos);
    col += vec3(0.95, 0.35, 0.15) * exp(-sun_d * 5.0) * 0.65;
    if (sun_d < 0.14) {
        float stripe = smoothstep(-0.02, 0.02, sin((p.y - horizon) * 80.0));
        col = mix(vec3(1.0, 0.65, 0.15), vec3(1.0, 0.20, 0.45), clamp((p.y - horizon) * 3.0, 0.0, 1.0));
        col *= mix(0.35, 1.0, stripe);
    }

    // Perspective speedway plane below horizon (Hor+ widens visible surroundings)
    if (p.y < horizon) {
        float depth = 0.45 / max(horizon - p.y, 0.005);
        float world_x = (p.x * depth) - u.stick_x * 1.4;
        float speed = 9.0 + (u.trigger_r - u.trigger_l) * 4.0;
        float world_z = depth + u.time_sec * speed;

        // Track surface vs out-of-bounds cyber grid
        float track_half = 1.65;
        float edge_dist = abs(world_x);
        vec3 floor_col = vec3(0.03, 0.04, 0.09);

        if (edge_dist < track_half) {
            floor_col = vec3(0.07, 0.09, 0.15);
            // Center dashed strip
            if (abs(world_x) < 0.06 && fract(world_z * 0.35) < 0.55) {
                floor_col = vec3(0.15, 0.78, 1.0);
            }
            // Boost pads every 8 units
            float pad_z = fract(world_z * 0.125);
            if (pad_z < 0.18 && abs(abs(world_x) - 0.85) < 0.28) {
                float chev = fract((world_z * 1.5) - abs(world_x) * 0.8);
                floor_col = mix(vec3(0.1, 0.9, 0.4), vec3(1.0, 0.95, 0.2), step(0.5, chev));
            }
            // Glowing guard rails
            if (edge_dist > track_half - 0.12) {
                floor_col = mix(vec3(0.0, 0.85, 1.0), vec3(1.0, 0.25, 0.6), step(0.5, fract(world_z * 0.5)));
            }
        } else {
            // Hor+ outer cyber grid
            float gx = abs(fract(world_x * 0.5) - 0.5);
            float gz = abs(fract(world_z * 0.5) - 0.5);
            float grid_line = 1.0 - smoothstep(0.0, 0.04, min(gx, gz));
            floor_col += vec3(0.14, 0.05, 0.32) * grid_line;
        }

        float fog = exp(-depth * 0.045);
        col = mix(vec3(0.08, 0.10, 0.24), floor_col, fog);
    }

    // Crisp 1-pixel calibration grid lines (helps verify 480p vs HD internal res & scanlines)
    vec2 px = v_uv * vec2(u.res_x, u.res_y);
    float minor_grid = step(mod(px.x, 32.0), 1.0) + step(mod(px.y, 32.0), 1.0);
    col += vec3(0.04, 0.05, 0.07) * clamp(minor_grid, 0.0, 1.0);

    // HUD Safe Area vs Edge Anchored indicator bars (L/R triggers + 4:3 safe box)
    float hud_half_x = mix((4.0 / 3.0) * 0.88, u.aspect_ratio * 0.94, u.hud_edge_anchored);
    float hud_box = abs(box_dist(p, vec2(hud_half_x, 0.88)));
    if (hud_box < 0.005) {
        col = mix(col, vec3(0.25, 0.92, 1.0), 0.75);
    }

    // Top-corner SMPTE calibration strip inside HUD region
    vec2 bar_origin = vec2(-hud_half_x + 0.04, 0.82);
    vec2 bar_size = vec2(0.42, 0.045);
    vec2 rel = p - bar_origin;
    if (rel.x >= 0.0 && rel.x <= bar_size.x && rel.y >= -bar_size.y && rel.y <= 0.0) {
        col = smpte_bar(rel.x / bar_size.x);
    }

    // Bottom L / R analog trigger gauges inside HUD region
    float bar_w = 0.36;
    vec2 l_origin = vec2(-hud_half_x + 0.04, -0.82);
    vec2 r_origin = vec2(hud_half_x - 0.04 - bar_w, -0.82);
    if (p.y >= l_origin.y && p.y <= l_origin.y + 0.04) {
        if (p.x >= l_origin.x && p.x <= l_origin.x + bar_w) {
            float fill = (p.x - l_origin.x) / bar_w;
            col = fill <= u.trigger_l ? vec3(0.2, 0.85, 1.0) : vec3(0.12, 0.14, 0.20);
        }
        if (p.x >= r_origin.x && p.x <= r_origin.x + bar_w) {
            float fill = (p.x - r_origin.x) / bar_w;
            col = fill <= u.trigger_r ? vec3(1.0, 0.45, 0.15) : vec3(0.12, 0.14, 0.20);
        }
    }

    // Status dot (green if ROM mounted, amber if calibration-only mode)
    vec2 dot_pos = vec2(hud_half_x - 0.06, 0.80);
    if (length(p - dot_pos) < 0.025) {
        col = u.rom_mounted > 0.5 ? vec3(0.15, 0.95, 0.40) : vec3(1.0, 0.72, 0.15);
    }

    out_color = vec4(col, 1.0);
}
