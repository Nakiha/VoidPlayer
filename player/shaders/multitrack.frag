#version 450 core
// MultiTrack fragment shader - multi-texture composition
// Supports SIDE_BY_SIDE (equal split) and SPLIT_SCREEN (overlapping with divider) modes
// Supports viewport zoom and pan

in vec2 v_texCoord;
in vec2 v_position;
out vec4 fragColor;

uniform int u_mode;           // 0 = SIDE_BY_SIDE, 1 = SPLIT_SCREEN
uniform int u_track_count;    // Number of active tracks
uniform float u_split_pos;    // Split position (0.0 - 1.0) for SPLIT_SCREEN mode
uniform int u_order[8];       // Track display order mapping

// 每个 track 的宽高比 (width / height), 0 表示使用画布比例
uniform float u_aspect_ratios[8];

// 每个 track 的分辨率尺寸 (像素)
uniform vec2 u_track_sizes[8];

// 画布宽高比
uniform float u_canvas_aspect;

// 画布尺寸 (像素)
uniform vec2 u_canvas_size;

// Viewport 缩放和偏移
uniform float u_zoom_ratio;   // 缩放比例: 1.0 = fit, >1.0 = 放大
uniform vec2 u_view_offset;   // 视图偏移 (片坐标系, 像素单位)

uniform sampler2D u_textures[8];  // Up to 8 texture slots

// 背景色 (letterbox/pillarbox)
const vec4 BG_COLOR = vec4(0.0, 0.0, 0.0, 1.0);

// 分割线颜色 (白色半透明)
const vec4 DIVIDER_COLOR = vec4(1.0, 1.0, 1.0, 0.8);

vec4 get_texture(int idx, vec2 uv) {
    return texture(u_textures[idx], uv);
}

// 计算 aspect-fit UV 坐标 (带缩放和偏移)
// slot_aspect: 分配的 slot 区域宽高比
// video_aspect: 视频宽高比
// video_size: 视频分辨率 (像素)
// local_uv: slot 内的本地 UV (0-1)
// zoom_ratio: 缩放比例
// view_offset: 视图偏移 (片坐标系, 像素单位)
// 返回: 视频纹理 UV，如果超出范围返回 alpha=0
vec2 calc_aspect_fit_uv(
    float slot_aspect,
    float video_aspect,
    vec2 video_size,
    vec2 local_uv,
    float zoom_ratio,
    vec2 view_offset,
    out bool out_of_bounds
) {
    out_of_bounds = false;

    if (video_aspect <= 0.0 || slot_aspect <= 0.0) {
        return local_uv;
    }

    // 1. 计算 fit 时的缩放因子
    float fit_scale;
    if (video_aspect > slot_aspect) {
        // 视频更宽，fit 高度
        fit_scale = 1.0 / video_aspect * slot_aspect;
    } else {
        // 视频更高，fit 宽度
        fit_scale = 1.0;
    }

    // 2. 应用 zoom (zoom_ratio=1.0 是 fit, >1.0 是放大)
    // 实际显示大小 = video_size * fit_scale * zoom_ratio
    float display_scale = fit_scale * zoom_ratio;

    // 3. 计算 display 区域在 slot 中的位置 (居中)
    vec2 display_size = vec2(
        video_aspect * display_scale / slot_aspect,
        display_scale
    );
    vec2 display_offset = (vec2(1.0) - display_size) * 0.5;

    // 4. 计算 display UV（不再检查边界，允许画面移出原始 display 区域）
    vec2 display_uv = local_uv - display_offset;

    // 5. 将 display UV 转换为片坐标 (0-1)
    // 注意：当 display_size 为 0 时会导致除零，需要保护
    vec2 normalized_uv = display_uv / max(display_size, vec2(0.0001));

    // 6. 应用 view_offset (片坐标系, 像素单位)
    // view_offset 是片坐标系中的偏移，需要转换到 UV 空间
    // X 取反：鼠标向右拖动，显示视频左侧内容
    // Y 不取反：后面有 Y 翻转，所以方向已经是正确的
    vec2 offset_uv = normalized_uv + vec2(-view_offset.x, view_offset.y) / video_size;

    // 7. 检查是否在视频范围内
    if (offset_uv.x < 0.0 || offset_uv.x > 1.0 ||
        offset_uv.y < 0.0 || offset_uv.y > 1.0) {
        out_of_bounds = true;
        return vec2(0.0);
    }

    // OpenGL Y 轴翻转: 纹理 Y=0 是顶部，我们习惯 Y=0 是底部
    offset_uv.y = 1.0 - offset_uv.y;

    return offset_uv;
}

void main() {
    int track_idx;
    vec2 local_uv;  // slot 内的本地 UV
    float slot_aspect;  // 当前 slot 的宽高比

    if (u_mode == 1) {
        // SPLIT_SCREEN mode: overlapping display with split divider
        // 两个视频在同一位置重叠，通过分割线切换显示
        if (v_texCoord.x < u_split_pos) {
            track_idx = u_order[0];  // 左侧显示 track 0
        } else {
            track_idx = u_order[1];  // 右侧显示 track 1
        }
        // UV 坐标不进行区域分割，使用完整画布坐标
        local_uv = v_texCoord;
        slot_aspect = u_canvas_aspect;  // slot 占满整个画布
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

    // 获取视频宽高比和尺寸
    float video_aspect = u_aspect_ratios[track_idx];
    vec2 video_size = u_track_sizes[track_idx];
    if (video_aspect <= 0.0) {
        video_aspect = slot_aspect;  // 默认使用 slot 比例
        video_size = vec2(slot_aspect, 1.0);  // 默认尺寸
    }

    // 计算 aspect-fit UV (带缩放和偏移)
    bool out_of_bounds;
    vec2 tex_uv = calc_aspect_fit_uv(
        slot_aspect, video_aspect, video_size,
        local_uv, u_zoom_ratio, u_view_offset,
        out_of_bounds
    );

    if (out_of_bounds) {
        fragColor = BG_COLOR;
    } else {
        fragColor = get_texture(track_idx, tex_uv);
    }

    // SPLIT_SCREEN 模式下渲染分割线 (2px 宽度)
    if (u_mode == 1 && u_canvas_size.x > 0.0) {
        float divider_x = u_split_pos * u_canvas_size.x;  // 分割线像素位置
        float pixel_x = v_texCoord.x * u_canvas_size.x;   // 当前像素 X 坐标
        float half_width = 1.0;  // 2px 总宽度，每侧 1px

        if (abs(pixel_x - divider_x) <= half_width) {
            // 在分割线范围内，混合分割线颜色
            float alpha = DIVIDER_COLOR.a * (1.0 - abs(pixel_x - divider_x) / half_width);
            fragColor = vec4(DIVIDER_COLOR.rgb, alpha) * alpha + fragColor * (1.0 - alpha);
        }
    }
}
