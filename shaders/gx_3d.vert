#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_uv0;
layout(location = 4) in vec2 in_uv1;

layout(set = 0, binding = 0) uniform GxCameraUbo {
    mat4 mvp;
    mat4 modelview;
    mat4 normal_mat;
    vec4 light_dir_intensity; // xyz = view-space light dir, w = intensity
    vec4 ambient_color;
    vec4 mat_color;
    uvec4 draw_flags;         // x: has_vtx_color, y: env_map_uv1, z: debug_mode, w: is_tex_preview
} u_cam;

layout(location = 0) out vec3 v_view_normal;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out float v_view_depth;

void main() {
    vec4 view_pos = u_cam.modelview * vec4(in_pos, 1.0);
    gl_Position = u_cam.mvp * vec4(in_pos, 1.0);

    vec3 n = mat3(u_cam.normal_mat) * in_normal;
    float nlen = length(n);
    v_view_normal = nlen > 1e-5 ? (n / nlen) : vec3(0.0, 1.0, 0.0);
    v_color = in_color;
    v_uv0 = in_uv0;
    v_uv1 = (u_cam.draw_flags.y != 0u) ? (v_view_normal.xy * 0.5 + 0.5) : in_uv1;
    v_view_depth = -view_pos.z;
}
