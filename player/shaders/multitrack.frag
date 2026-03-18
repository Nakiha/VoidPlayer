#version 330 core
// MultiTrack fragment shader - multi-texture composition
// Supports SIDE_BY_SIDE (equal split) and SPLIT (draggable divider) modes

in vec2 v_texCoord;
in vec2 v_position;
out vec4 fragColor;

uniform int u_mode;           // 0 = SIDE_BY_SIDE, 1 = SPLIT
uniform int u_track_count;    // Number of active tracks
uniform float u_split_pos;    // Split position (0.0 - 1.0) for SPLIT mode
uniform int u_order[8];       // Track display order mapping

// 每个 track 的宽高比 (width / height), 0 表示使用画布比例
uniform float u_aspect_ratios[8];

// 画布宽高比
uniform float u_canvas_aspect;

uniform sampler2D u_textures[8];  // Up to 8 texture slots

// 背景色 (letterbox/pillarbox)
const vec4 BG_COLOR = vec4(0.0, 0.0, 0.0, 1.0);

vec4 get_texture(int idx, vec2 uv) {
    if (idx == 0) return texture(u_textures[0], uv);
    else if (idx == 1) return texture(u_textures[1], uv);
    else if (idx == 2) return texture(u_textures[2], uv);
    else if (idx == 3) return texture(u_textures[3], uv);
    else if (idx == 4) return texture(u_textures[4], uv);
    else if (idx == 5) return texture(u_textures[5], uv);
    else if (idx == 6) return texture(u_textures[6], uv);
    else return texture(u_textures[7], uv);
}

// 计算 aspect-fit UV 坐标
// slot_aspect: 分配的 slot 区域宽高比
// video_aspect: 视频宽高比
// local_uv: slot 内的本地 UV (0-1)
// 返回: 视频纹理 UV，如果超出范围返回 alpha=0
vec2 calc_aspect_fit_uv(float slot_aspect, float video_aspect, vec2 local_uv, out bool out_of_bounds) {
    out_of_bounds = false;

    if (video_aspect <= 0.0 || slot_aspect <= 0.0) {
        return local_uv;
    }

    vec2 uv = local_uv;

    if (video_aspect > slot_aspect) {
        // 视频更宽，需要上下 letterbox
        float scale = slot_aspect / video_aspect;
        float offset = (1.0 - scale) * 0.5;

        if (local_uv.y < offset || local_uv.y > offset + scale) {
            out_of_bounds = true;
            return vec2(0.0);
        }

        uv.x = local_uv.x;
        uv.y = (local_uv.y - offset) / scale;
    } else {
        // 视频更高，需要左右 pillarbox
        float scale = video_aspect / slot_aspect;
        float offset = (1.0 - scale) * 0.5;

        if (local_uv.x < offset || local_uv.x > offset + scale) {
            out_of_bounds = true;
            return vec2(0.0);
        }

        uv.x = (local_uv.x - offset) / scale;
        uv.y = local_uv.y;
    }

    // OpenGL Y 轴翻转: 纹理 Y=0 是顶部，我们习惯 Y=0 是底部
    uv.y = 1.0 - uv.y;

    return uv;
}

void main() {
    int track_idx;
    vec2 local_uv;  // slot 内的本地 UV
    float slot_aspect;  // 当前 slot 的宽高比

    if (u_mode == 1) {
        // SPLIT mode: show first 2 tracks with draggable divider
        if (v_texCoord.x < u_split_pos) {
            track_idx = u_order[0];
            local_uv = vec2(v_texCoord.x / max(u_split_pos, 0.001), v_texCoord.y);
            // slot 宽高比 = canvas_aspect * split_pos
            slot_aspect = u_canvas_aspect * u_split_pos;
        } else {
            track_idx = u_order[1];
            float remain = max(1.0 - u_split_pos, 0.001);
            local_uv = vec2((v_texCoord.x - u_split_pos) / remain, v_texCoord.y);
            slot_aspect = u_canvas_aspect * remain;
        }
    } else {
        // SIDE_BY_SIDE mode: equal width 1/N
        int count = max(u_track_count, 1);
        int slot = int(v_texCoord.x * float(count));
        slot = clamp(slot, 0, count - 1);
        track_idx = u_order[slot];
        local_uv = vec2(v_texCoord.x * float(count) - float(slot), v_texCoord.y);
        // slot 宽高比 = canvas_aspect / N
        slot_aspect = u_canvas_aspect / float(count);
    }

    // 获取视频宽高比
    float video_aspect = u_aspect_ratios[track_idx];
    if (video_aspect <= 0.0) {
        video_aspect = slot_aspect;  // 默认使用 slot 比例
    }

    // 计算 aspect-fit UV
    bool out_of_bounds;
    vec2 tex_uv = calc_aspect_fit_uv(slot_aspect, video_aspect, local_uv, out_of_bounds);

    if (out_of_bounds) {
        fragColor = BG_COLOR;
    } else {
        fragColor = get_texture(track_idx, tex_uv);
    }
}
