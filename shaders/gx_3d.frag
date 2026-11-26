#version 450

layout(location = 0) in vec3 v_view_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in float v_view_depth;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform GxCameraUbo {
    mat4 mvp;
    mat4 modelview;
    mat4 normal_mat;
    vec4 light_dir_intensity;
    vec4 ambient_color;
    vec4 mat_color;
    uvec4 draw_flags;         // x: has_vtx_color, y: env_map_uv1, z: debug_mode, w: is_tex_preview
} u_cam;

layout(set = 0, binding = 1) uniform GxTevUbo {
    uvec4 counts_and_alpha;   // x: num_stages, y: comp0 | (comp1<<4) | (alpha_op<<8), z: fog_type, w: flags (b0:tex, b1:lit, b2:fog)
    vec4 alpha_refs_fog;      // x: ref0, y: ref1, z: fog_start_z, w: fog_end_z
    vec4 fog_color;
    vec4 tev_regs[4];
    vec4 konst_regs[4];
    uvec4 swap_tables[4];
    uvec4 stage_color[16];    // x: a|(b<<8)|(c<<16)|(d<<24), y: op|(bias<<4)|(scale<<8)|(clamp<<12)|(out_reg<<16), z: tex_map|(tex_coord<<8)|(ras_chan<<16)|(kcolor_sel<<24), w: ras_swap|(tex_swap<<8)
    uvec4 stage_alpha[16];    // x: a|(b<<8)|(c<<16)|(d<<24), y: op|(bias<<4)|(scale<<8)|(clamp<<12)|(out_reg<<16), z: kalpha_sel, w: 0
} u_tev;

layout(set = 1, binding = 0) uniform sampler2D u_tex[4];

vec4 sample_tex_slot(uint slot, vec2 uv) {
    if (slot == 0u) return texture(u_tex[0], uv);
    if (slot == 1u) return texture(u_tex[1], uv);
    if (slot == 2u) return texture(u_tex[2], uv);
    if (slot == 3u) return texture(u_tex[3], uv);
    return vec4(1.0);
}

vec4 apply_swap(vec4 c, uvec4 sw) {
    float arr[4] = float[4](c.r, c.g, c.b, c.a);
    return vec4(arr[min(sw.x, 3u)], arr[min(sw.y, 3u)], arr[min(sw.z, 3u)], arr[min(sw.w, 3u)]);
}

vec3 eval_konst_rgb(uint sel) {
    if (sel <= 7u) {
        float f = float(8u - sel) / 8.0;
        return vec3(f);
    }
    if (sel >= 12u && sel <= 15u) return u_tev.konst_regs[sel - 12u].rgb;
    if (sel >= 16u && sel <= 19u) return vec3(u_tev.konst_regs[sel - 16u].r);
    if (sel >= 20u && sel <= 23u) return vec3(u_tev.konst_regs[sel - 20u].g);
    if (sel >= 24u && sel <= 27u) return vec3(u_tev.konst_regs[sel - 24u].b);
    if (sel >= 28u && sel <= 31u) return vec3(u_tev.konst_regs[sel - 28u].a);
    return vec3(1.0);
}

float eval_konst_alpha(uint sel) {
    if (sel <= 7u) return float(8u - sel) / 8.0;
    if (sel >= 16u && sel <= 19u) return u_tev.konst_regs[sel - 16u].r;
    if (sel >= 20u && sel <= 23u) return u_tev.konst_regs[sel - 20u].g;
    if (sel >= 24u && sel <= 27u) return u_tev.konst_regs[sel - 24u].b;
    if (sel >= 28u && sel <= 31u) return u_tev.konst_regs[sel - 28u].a;
    return 1.0;
}

vec3 pick_color_arg(uint arg, vec4 regs[4], vec4 tex, vec4 ras, vec3 konst) {
    if (arg == 0u) return regs[0].rgb;
    if (arg == 1u) return vec3(regs[0].a);
    if (arg == 2u) return regs[1].rgb;
    if (arg == 3u) return vec3(regs[1].a);
    if (arg == 4u) return regs[2].rgb;
    if (arg == 5u) return vec3(regs[2].a);
    if (arg == 6u) return regs[3].rgb;
    if (arg == 7u) return vec3(regs[3].a);
    if (arg == 8u) return tex.rgb;
    if (arg == 9u) return vec3(tex.a);
    if (arg == 10u) return ras.rgb;
    if (arg == 11u) return vec3(ras.a);
    if (arg == 12u) return vec3(1.0);
    if (arg == 13u) return vec3(0.5);
    if (arg == 14u) return konst;
    return vec3(0.0);
}

float pick_alpha_arg(uint arg, vec4 regs[4], vec4 tex, vec4 ras, float konst_a) {
    if (arg == 0u) return regs[0].a;
    if (arg == 1u) return regs[1].a;
    if (arg == 2u) return regs[2].a;
    if (arg == 3u) return regs[3].a;
    if (arg == 4u) return tex.a;
    if (arg == 5u) return ras.a;
    if (arg == 6u) return konst_a;
    return 0.0;
}

float bias_val(uint b) {
    if (b == 1u) return 0.5;
    if (b == 2u) return -0.5;
    return 0.0;
}

float scale_val(uint s) {
    if (s == 1u) return 2.0;
    if (s == 2u) return 4.0;
    if (s == 3u) return 0.5;
    return 1.0;
}

bool eval_compare(float a, float ref_v, uint cmp) {
    if (cmp == 0u) return false;
    if (cmp == 1u) return a < ref_v;
    if (cmp == 2u) return abs(a - ref_v) < (0.5 / 255.0);
    if (cmp == 3u) return a <= ref_v;
    if (cmp == 4u) return a > ref_v;
    if (cmp == 5u) return abs(a - ref_v) >= (0.5 / 255.0);
    if (cmp == 6u) return a >= ref_v;
    return true;
}

void main() {
    if (u_cam.draw_flags.w != 0u) {
        vec4 t0 = texture(u_tex[0], v_uv0);
        vec2 cb = floor(v_uv0 * 16.0);
        float chk = mod(cb.x + cb.y, 2.0) * 0.14 + 0.14;
        out_color = vec4(mix(vec3(chk), t0.rgb, t0.a), 1.0);
        return;
    }

    uint debug_mode = u_cam.draw_flags.z;
    if (debug_mode == 2u) {
        out_color = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
        return;
    }

    vec4 base = (u_cam.draw_flags.x != 0u) ? (v_color * u_cam.mat_color) : u_cam.mat_color;
    if (debug_mode == 3u) {
        out_color = base;
        return;
    }

    bool enable_tex = (u_tev.counts_and_alpha.w & 1u) != 0u;
    bool enable_lit = (u_tev.counts_and_alpha.w & 2u) != 0u && debug_mode == 0u;
    bool enable_fog = (u_tev.counts_and_alpha.w & 4u) != 0u && u_tev.counts_and_alpha.z != 0u;

    vec4 ras_lit = base;
    if (enable_lit && u_cam.light_dir_intensity.w > 0.001) {
        vec3 n = normalize(v_view_normal);
        vec3 l = normalize(u_cam.light_dir_intensity.xyz);
        float ndotl = max(dot(n, l), 0.0);
        float hemi = 0.5 + 0.5 * n.y;
        vec3 amb = mix(u_cam.ambient_color.rgb * 0.65, u_cam.ambient_color.rgb * 1.15, hemi);
        vec3 lit = amb + vec3(0.85) * ndotl * u_cam.light_dir_intensity.w;
        ras_lit.rgb *= clamp(lit, vec3(0.15), vec3(1.6));
    }

    vec4 regs[4] = vec4[4](
        u_tev.tev_regs[0],
        u_tev.tev_regs[1],
        u_tev.tev_regs[2],
        u_tev.tev_regs[3]
    );

    uint num_stages = clamp(u_tev.counts_and_alpha.x, 1u, 16u);
    for (uint s = 0u; s < num_stages; ++s) {
        uvec4 sc = u_tev.stage_color[s];
        uvec4 sa = u_tev.stage_alpha[s];

        uint tex_map = sc.z & 0xFFu;
        uint tex_coord = (sc.z >> 8) & 0xFFu;
        uint ras_sw = min(sc.w & 0xFFu, 3u);
        uint tex_sw = min((sc.w >> 8) & 0xFFu, 3u);

        vec4 ras = apply_swap(ras_lit, u_tev.swap_tables[ras_sw]);
        vec4 tex = vec4(1.0);
        if (enable_tex && tex_map < 4u) {
            vec2 uv = (tex_coord == 1u) ? v_uv1 : v_uv0;
            tex = apply_swap(sample_tex_slot(tex_map, uv), u_tev.swap_tables[tex_sw]);
        }

        vec3 kc = eval_konst_rgb((sc.z >> 24) & 0xFFu);
        float ka = eval_konst_alpha(sa.z & 0xFFu);

        vec3 ca = pick_color_arg(sc.x & 0xFFu, regs, tex, ras, kc);
        vec3 cb = pick_color_arg((sc.x >> 8) & 0xFFu, regs, tex, ras, kc);
        vec3 cc = pick_color_arg((sc.x >> 16) & 0xFFu, regs, tex, ras, kc);
        vec3 cd = pick_color_arg((sc.x >> 24) & 0xFFu, regs, tex, ras, kc);

        float csgn = ((sc.y >> 2) & 1u) != 0u ? -1.0 : 1.0; // wait: op is bit 0, let's match C++ packing!
        csgn = (sc.y & 1u) != 0u ? -1.0 : 1.0;
        float cbias = bias_val((sc.y >> 4) & 3u);
        float cscale = scale_val((sc.y >> 8) & 3u);
        bool cclamp = ((sc.y >> 12) & 1u) != 0u;
        uint cout_reg = min((sc.y >> 16) & 3u, 3u);

        vec3 cout = (cd + csgn * mix(ca, cb, cc) + vec3(cbias)) * cscale;
        if (cclamp) cout = clamp(cout, vec3(0.0), vec3(1.0));

        float aa = pick_alpha_arg(sa.x & 0xFFu, regs, tex, ras, ka);
        float ab = pick_alpha_arg((sa.x >> 8) & 0xFFu, regs, tex, ras, ka);
        float ac = pick_alpha_arg((sa.x >> 16) & 0xFFu, regs, tex, ras, ka);
        float ad = pick_alpha_arg((sa.x >> 24) & 0xFFu, regs, tex, ras, ka);

        float asgn = (sa.y & 1u) != 0u ? -1.0 : 1.0;
        float abias = bias_val((sa.y >> 4) & 3u);
        float ascale = scale_val((sa.y >> 8) & 3u);
        bool aclamp = ((sa.y >> 12) & 1u) != 0u;
        uint aout_reg = min((sa.y >> 16) & 3u, 3u);

        float aout = (ad + asgn * mix(aa, ab, ac) + abias) * ascale;
        if (aclamp) aout = clamp(aout, 0.0, 1.0);

        regs[cout_reg].rgb = cout;
        regs[aout_reg].a = aout;
    }

    vec4 color = regs[0];

    // Alpha compare
    uint comp0 = u_tev.counts_and_alpha.y & 0xFu;
    uint comp1 = (u_tev.counts_and_alpha.y >> 4) & 0xFu;
    uint aop = (u_tev.counts_and_alpha.y >> 8) & 3u;
    bool pass0 = eval_compare(color.a, u_tev.alpha_refs_fog.x, comp0);
    bool pass1 = eval_compare(color.a, u_tev.alpha_refs_fog.y, comp1);
    bool alpha_pass = true;
    if (aop == 0u) alpha_pass = pass0 && pass1;
    else if (aop == 1u) alpha_pass = pass0 || pass1;
    else if (aop == 2u) alpha_pass = pass0 != pass1;
    else alpha_pass = pass0 == pass1;
    if (!alpha_pass) {
        discard;
    }

    // Fog
    if (enable_fog) {
        float fog_t = clamp(
            (v_view_depth - u_tev.alpha_refs_fog.z) / max(0.001, u_tev.alpha_refs_fog.w - u_tev.alpha_refs_fog.z),
            0.0, 1.0);
        uint ftype = u_tev.counts_and_alpha.z;
        float fog_f = fog_t;
        if (ftype == 4u || ftype == 12u) {
            fog_f = 1.0 - exp2(-8.0 * fog_t);
        } else if (ftype == 5u) {
            fog_f = 1.0 - exp2(-8.0 * fog_t * fog_t);
        } else if (ftype == 6u) {
            fog_f = exp2(-8.0 * (1.0 - fog_t));
        } else if (ftype == 7u) {
            float u = 1.0 - fog_t;
            fog_f = exp2(-8.0 * u * u);
        }
        color.rgb = mix(color.rgb, u_tev.fog_color.rgb, clamp(fog_f, 0.0, 1.0));
    }

    out_color = color;
}
