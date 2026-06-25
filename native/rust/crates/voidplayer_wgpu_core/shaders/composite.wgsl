struct CompositeParams {
  target_mode: vec4<f32>,
  split: vec4<f32>,
  background: vec4<f32>,
  order: vec4<i32>,
  display_offset_x: vec4<f32>,
  display_offset_y: vec4<f32>,
  inv_display_size_x: vec4<f32>,
  inv_display_size_y: vec4<f32>,
  view_offset_uv_x: vec4<f32>,
  view_offset_uv_y: vec4<f32>,
  present: vec4<i32>,
  source_width: vec4<f32>,
  source_height: vec4<f32>,
  yuv_format: vec4<i32>,
  y_offset: vec4<i32>,
  uv_offset: vec4<i32>,
  v_offset: vec4<i32>,
  y_stride: vec4<i32>,
  uv_stride: vec4<i32>,
  coded_width: vec4<i32>,
  coded_height: vec4<i32>,
  color_range: vec4<i32>,
  color_matrix: vec4<i32>,
  overlay_counts: vec4<i32>,
  color_transfer: vec4<i32>,
  color_primaries: vec4<i32>,
  output_mode: vec4<i32>,
};

struct OverlayRect {
  rect_uv0: u32,
  rect_uv1: u32,
  color_bgra: u32,
  track_idx: u32,
};

@group(0) @binding(0)
var<storage, read> params: CompositeParams;

@group(0) @binding(1)
var src_texture: texture_2d_array<f32>;

@group(0) @binding(2)
var src_sampler: sampler;

@group(0) @binding(3)
var<storage, read> package_words: array<u32>;

@group(0) @binding(4)
var<storage, read> overlay_rects: array<OverlayRect>;

@group(0) @binding(5)
var cv_y0: texture_2d<f32>;

@group(0) @binding(6)
var cv_uv0: texture_2d<f32>;

@group(0) @binding(7)
var cv_y1: texture_2d<f32>;

@group(0) @binding(8)
var cv_uv1: texture_2d<f32>;

@group(0) @binding(9)
var cv_y2: texture_2d<f32>;

@group(0) @binding(10)
var cv_uv2: texture_2d<f32>;

@group(0) @binding(11)
var cv_y3: texture_2d<f32>;

@group(0) @binding(12)
var cv_uv3: texture_2d<f32>;

@group(0) @binding(13)
var overlay_layer_texture: texture_2d_array<f32>;

struct VertexOut {
  @builtin(position) position: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOut {
  var positions = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -3.0),
    vec2<f32>(-1.0, 1.0),
    vec2<f32>(3.0, 1.0),
  );
  var out: VertexOut;
  out.position = vec4<f32>(positions[vertex_index], 0.0, 1.0);
  return out;
}

fn vec4_get_i(values: vec4<i32>, index: i32) -> i32 {
  switch index {
    case 0: { return values.x; }
    case 1: { return values.y; }
    case 2: { return values.z; }
    default: { return values.w; }
  }
}

fn vec4_get_f(values: vec4<f32>, index: i32) -> f32 {
  switch index {
    case 0: { return values.x; }
    case 1: { return values.y; }
    case 2: { return values.z; }
    default: { return values.w; }
  }
}

const COLOR_TRANSFER_SDR: i32 = 1;
const COLOR_TRANSFER_PQ: i32 = 2;
const COLOR_TRANSFER_HLG: i32 = 3;
const COLOR_PRIMARIES_BT601: i32 = 1;
const COLOR_PRIMARIES_BT2020: i32 = 3;
const OUTPUT_COLOR_MODE_EDR: i32 = 2;
const HDR_REFERENCE_WHITE_NITS: f32 = 203.0;
const HLG_EDR_HEADROOM_SCALE: f32 = 4.0;

fn output_is_edr() -> bool {
  return params.output_mode.x == OUTPUT_COLOR_MODE_EDR;
}

fn color_transfer_at(track: i32) -> i32 {
  return vec4_get_i(params.color_transfer, track);
}

fn color_primaries_at(track: i32) -> i32 {
  return vec4_get_i(params.color_primaries, track);
}

fn linear_to_srgb(x: vec3<f32>) -> vec3<f32> {
  let v = max(x, vec3<f32>(0.0));
  let lo = v * 12.92;
  let hi = 1.055 * pow(v, vec3<f32>(1.0 / 2.4)) - vec3<f32>(0.055);
  return mix(lo, hi, step(vec3<f32>(0.0031308), v));
}

fn srgb_to_linear(x: vec3<f32>) -> vec3<f32> {
  let v = clamp(x, vec3<f32>(0.0), vec3<f32>(1.0));
  let lo = v / 12.92;
  let hi = pow((v + vec3<f32>(0.055)) / 1.055, vec3<f32>(2.4));
  return mix(lo, hi, step(vec3<f32>(0.04045), v));
}

fn convert_linear_bt2020_to_display_p3(rgb: vec3<f32>) -> vec3<f32> {
  return vec3<f32>(
      1.3435782526 * rgb.r - 0.2821796705 * rgb.g - 0.0613985821 * rgb.b,
     -0.0652974528 * rgb.r + 1.0757879158 * rgb.g - 0.0104904631 * rgb.b,
      0.0028217873 * rgb.r - 0.0195984945 * rgb.g + 1.0167767073 * rgb.b);
}

fn convert_linear_bt709_to_display_p3(rgb: vec3<f32>) -> vec3<f32> {
  return vec3<f32>(
      0.8224619687 * rgb.r + 0.1775380313 * rgb.g,
      0.0331941989 * rgb.r + 0.9668058011 * rgb.g,
      0.0170826307 * rgb.r + 0.0723974407 * rgb.g + 0.9105199286 * rgb.b);
}

fn convert_linear_bt601_to_display_p3(rgb: vec3<f32>) -> vec3<f32> {
  return vec3<f32>(
      0.7758928495 * rgb.r + 0.2127372197 * rgb.g + 0.0113699286 * rgb.b,
      0.0483696384 * rgb.r + 0.9353998726 * rgb.g + 0.0162304897 * rgb.b,
      0.0158600140 * rgb.r + 0.0667994164 * rgb.g + 0.9173405701 * rgb.b);
}

fn convert_linear_primaries_to_display_p3(rgb: vec3<f32>, primaries: i32) -> vec3<f32> {
  if (primaries == COLOR_PRIMARIES_BT2020) {
    return convert_linear_bt2020_to_display_p3(rgb);
  }
  if (primaries == COLOR_PRIMARIES_BT601) {
    return convert_linear_bt601_to_display_p3(rgb);
  }
  return convert_linear_bt709_to_display_p3(rgb);
}

fn convert_linear_primaries_to_bt709(rgb: vec3<f32>, primaries: i32) -> vec3<f32> {
  if (primaries == COLOR_PRIMARIES_BT2020) {
    return vec3<f32>(
        1.6605 * rgb.r - 0.5876 * rgb.g - 0.0728 * rgb.b,
       -0.1246 * rgb.r + 1.1329 * rgb.g - 0.0083 * rgb.b,
       -0.0182 * rgb.r - 0.1006 * rgb.g + 1.1187 * rgb.b);
  }
  return rgb;
}

fn pq_to_linear_nits(x: vec3<f32>) -> vec3<f32> {
  let v = clamp(x, vec3<f32>(0.0), vec3<f32>(1.0));
  let m1 = 0.1593017578125;
  let m2 = 78.84375;
  let c1 = 0.8359375;
  let c2 = 18.8515625;
  let c3 = 18.6875;
  let p = pow(v, vec3<f32>(1.0 / m2));
  let num = max(p - vec3<f32>(c1), vec3<f32>(0.0));
  let den = max(vec3<f32>(c2) - vec3<f32>(c3) * p, vec3<f32>(0.000001));
  return pow(num / den, vec3<f32>(1.0 / m1)) * 10000.0;
}

fn hlg_to_linear(x: vec3<f32>) -> vec3<f32> {
  let v = clamp(x, vec3<f32>(0.0), vec3<f32>(1.0));
  let a = 0.17883277;
  let b = 0.28466892;
  let c = 0.55991073;
  let lo = (v * v) / 3.0;
  let hi = (exp((v - vec3<f32>(c)) / a) + vec3<f32>(b)) / 12.0;
  return mix(lo, hi, step(vec3<f32>(0.5), v));
}

fn tone_map_to_sdr(rgb: vec3<f32>, transfer: i32, primaries: i32) -> vec3<f32> {
  if (transfer == COLOR_TRANSFER_PQ) {
    var lin = pq_to_linear_nits(rgb) / HDR_REFERENCE_WHITE_NITS;
    lin = convert_linear_primaries_to_bt709(lin, primaries);
    return clamp(linear_to_srgb(lin / (vec3<f32>(1.0) + lin)), vec3<f32>(0.0), vec3<f32>(1.0));
  }
  if (transfer == COLOR_TRANSFER_HLG) {
    var lin = hlg_to_linear(rgb) * HLG_EDR_HEADROOM_SCALE;
    lin = convert_linear_primaries_to_bt709(lin, primaries);
    return clamp(linear_to_srgb(lin / (vec3<f32>(1.0) + lin)), vec3<f32>(0.0), vec3<f32>(1.0));
  }
  if (primaries == COLOR_PRIMARIES_BT2020) {
    let lin = convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries);
    return clamp(linear_to_srgb(lin), vec3<f32>(0.0), vec3<f32>(1.0));
  }
  return clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0));
}

fn map_to_edr(rgb: vec3<f32>, transfer: i32, primaries: i32) -> vec3<f32> {
  if (transfer == COLOR_TRANSFER_PQ) {
    let lin = pq_to_linear_nits(rgb) / HDR_REFERENCE_WHITE_NITS;
    return max(convert_linear_primaries_to_display_p3(lin, primaries), vec3<f32>(0.0));
  }
  if (transfer == COLOR_TRANSFER_HLG) {
    let lin = hlg_to_linear(rgb) * HLG_EDR_HEADROOM_SCALE;
    return max(convert_linear_primaries_to_display_p3(lin, primaries), vec3<f32>(0.0));
  }
  return max(convert_linear_primaries_to_display_p3(srgb_to_linear(rgb), primaries), vec3<f32>(0.0));
}

fn map_source_to_output(color: vec4<f32>, track: i32) -> vec4<f32> {
  let transfer = color_transfer_at(track);
  let primaries = color_primaries_at(track);
  if (output_is_edr()) {
    return vec4<f32>(map_to_edr(color.rgb, transfer, primaries), color.a);
  }
  return vec4<f32>(tone_map_to_sdr(color.rgb, transfer, primaries), color.a);
}

fn map_sdr_ui_to_output(color: vec4<f32>) -> vec4<f32> {
  let ui = clamp(color, vec4<f32>(0.0), vec4<f32>(1.0));
  if (!output_is_edr()) {
    return ui;
  }
  return vec4<f32>(convert_linear_bt709_to_display_p3(srgb_to_linear(ui.rgb)), ui.a);
}

fn select_track(tex_uv: vec2<f32>) -> vec2<i32> {
  let mode = i32(round(params.target_mode.z));
  let track_count = max(1, min(i32(round(params.target_mode.w)), 4));
  if (mode == 1) {
    let split = clamp(params.split.x, 0.0, 1.0);
    let order_index = select(1, 0, tex_uv.x < split);
    return vec2<i32>(vec4_get_i(params.order, order_index), order_index);
  }
  let scaled_x = tex_uv.x * f32(track_count);
  let order_index = min(i32(scaled_x), track_count - 1);
  return vec2<i32>(vec4_get_i(params.order, order_index), order_index);
}

fn track_local_uv(tex_uv: vec2<f32>, order_index: i32) -> vec2<f32> {
  let mode = i32(round(params.target_mode.z));
  if (mode == 1) {
    return tex_uv;
  }
  let track_count = max(1, min(i32(round(params.target_mode.w)), 4));
  let scaled_x = tex_uv.x * f32(track_count);
  return vec2<f32>(fract(scaled_x), tex_uv.y);
}

fn source_uv(track: i32, local_uv: vec2<f32>) -> vec2<f32> {
  return vec2<f32>(
    (local_uv.x - vec4_get_f(params.display_offset_x, track)) *
        vec4_get_f(params.inv_display_size_x, track) -
        vec4_get_f(params.view_offset_uv_x, track),
    (local_uv.y - vec4_get_f(params.display_offset_y, track)) *
        vec4_get_f(params.inv_display_size_y, track) -
        vec4_get_f(params.view_offset_uv_y, track),
  );
}

fn apply_split_divider(color: vec4<f32>, tex_x: f32) -> vec4<f32> {
  let mode = i32(round(params.target_mode.z));
  if (mode != 1) {
    return color;
  }
  let divider_x = clamp(params.split.x, 0.0, 1.0) * params.target_mode.x;
  let dist = abs(tex_x - divider_x);
  let core_width = 1.25;
  let edge_width = 0.75;
  if (dist > core_width + edge_width) {
    return color;
  }
  let alpha = select(1.0 - ((dist - core_width) / edge_width), 1.0, dist <= core_width);
  return vec4<f32>(mix(color.rgb, 1.0 - color.rgb, alpha), color.a);
}

fn overlay_track_index(rect: OverlayRect) -> i32 {
  return clamp(i32(rect.track_idx & 0xffu), 0, 3);
}

fn overlay_line_strength(rect: OverlayRect) -> f32 {
  return f32((rect.track_idx >> 8u) & 0xffu) / 255.0;
}

fn overlay_display_slot_for_track(track: i32) -> i32 {
  if (params.order.x == track) {
    return 0;
  }
  if (params.order.y == track) {
    return 1;
  }
  if (params.order.z == track) {
    return 2;
  }
  return 3;
}

fn overlay_unpack_uv16(packed: u32) -> vec2<f32> {
  return vec2<f32>(
    f32(packed & 0xffffu) / 65535.0,
    f32((packed >> 16u) & 0xffffu) / 65535.0,
  );
}

fn overlay_local_from_video(video_uv: vec2<f32>, track: i32) -> vec2<f32> {
  let inv_display_size = vec2<f32>(
    vec4_get_f(params.inv_display_size_x, track),
    vec4_get_f(params.inv_display_size_y, track),
  );
  let display_size = vec2<f32>(
    select(0.0, 1.0 / inv_display_size.x, abs(inv_display_size.x) > 0.00001),
    select(0.0, 1.0 / inv_display_size.y, abs(inv_display_size.y) > 0.00001),
  );
  let display_offset = vec2<f32>(
    vec4_get_f(params.display_offset_x, track),
    vec4_get_f(params.display_offset_y, track),
  );
  let view_offset = vec2<f32>(
    vec4_get_f(params.view_offset_uv_x, track),
    vec4_get_f(params.view_offset_uv_y, track),
  );
  return display_offset + (video_uv + view_offset) * display_size;
}

fn overlay_visible_local_rect_for_track(track: i32) -> vec4<f32> {
  var visible_min = vec2<f32>(0.0, 0.0);
  var visible_max = vec2<f32>(1.0, 1.0);
  let mode = i32(round(params.target_mode.z));
  if (mode == 1) {
    let slot = overlay_display_slot_for_track(track);
    if (slot == 0) {
      visible_max.x = clamp(params.split.x, 0.0, 1.0);
    } else if (slot == 1) {
      visible_min.x = clamp(params.split.x, 0.0, 1.0);
    } else {
      visible_max = visible_min;
    }
  }
  return vec4<f32>(visible_min, visible_max);
}

fn overlay_global_from_local(local_uv: vec2<f32>, track: i32) -> vec2<f32> {
  let mode = i32(round(params.target_mode.z));
  if (mode == 1) {
    return local_uv;
  }
  let track_count = max(1, min(i32(round(params.target_mode.w)), 4));
  let slot = clamp(overlay_display_slot_for_track(track), 0, track_count - 1);
  return vec2<f32>((f32(slot) + local_uv.x) / f32(track_count), local_uv.y);
}

fn overlay_global_rect(rect: OverlayRect) -> vec4<f32> {
  let track = overlay_track_index(rect);
  let local_a = overlay_local_from_video(overlay_unpack_uv16(rect.rect_uv0), track);
  let local_b = overlay_local_from_video(overlay_unpack_uv16(rect.rect_uv1), track);
  let visible = overlay_visible_local_rect_for_track(track);
  let clipped_min = max(min(local_a, local_b), visible.xy);
  let clipped_max = min(max(local_a, local_b), visible.zw);
  let global_min = overlay_global_from_local(clipped_min, track);
  let global_max = overlay_global_from_local(clipped_max, track);
  return vec4<f32>(min(global_min, global_max), max(global_min, global_max));
}

fn overlay_color_from_bgra(color_bgra: u32) -> vec4<f32> {
  return vec4<f32>(
    f32((color_bgra >> 16u) & 0xffu),
    f32((color_bgra >> 8u) & 0xffu),
    f32(color_bgra & 0xffu),
    f32((color_bgra >> 24u) & 0xffu),
  ) / 255.0;
}

fn overlay_blend_over(dst: vec4<f32>, src: vec4<f32>) -> vec4<f32> {
  let alpha = clamp(src.a, 0.0, 1.0);
  return vec4<f32>(
    src.rgb * alpha + dst.rgb * (1.0 - alpha),
    alpha + dst.a * (1.0 - alpha),
  );
}

fn overlay_rect_matches_track(rect: OverlayRect, track: i32) -> bool {
  return overlay_track_index(rect) == track;
}

fn overlay_layer_rect(rect: OverlayRect) -> vec4<f32> {
  let a = overlay_unpack_uv16(rect.rect_uv0);
  let b = overlay_unpack_uv16(rect.rect_uv1);
  return vec4<f32>(min(a, b), max(a, b));
}

fn overlay_layer_apply_fill(color: vec4<f32>, video_uv: vec2<f32>, track: i32) -> vec4<f32> {
  var out = color;
  let count = max(0, params.overlay_counts.x);
  for (var i = 0; i < count; i = i + 1) {
    let rect = overlay_rects[u32(i)];
    if (!overlay_rect_matches_track(rect, track)) {
      continue;
    }
    let r = overlay_layer_rect(rect);
    if (video_uv.x >= r.x && video_uv.x <= r.z &&
        video_uv.y >= r.y && video_uv.y <= r.w) {
      out = overlay_blend_over(out, overlay_color_from_bgra(rect.color_bgra));
    }
  }
  return out;
}

fn overlay_layer_apply_lines(color: vec4<f32>, video_uv: vec2<f32>, track: i32) -> vec4<f32> {
  var out = color;
  let target_size = vec2<f32>(params.target_mode.x, params.target_mode.y);
  let px = video_uv * target_size;
  let count = max(0, params.overlay_counts.y);
  for (var i = 0; i < count; i = i + 1) {
    let rect = overlay_rects[u32(params.overlay_counts.x + i)];
    if (!overlay_rect_matches_track(rect, track) || overlay_line_strength(rect) <= 0.0) {
      continue;
    }
    let r = overlay_layer_rect(rect);
    let min_px = r.xy * target_size;
    let max_px = r.zw * target_size;
    let inside = px.x >= min_px.x && px.x <= max_px.x &&
        px.y >= min_px.y && px.y <= max_px.y;
    let edge_dist = min(
      min(abs(px.x - min_px.x), abs(px.x - max_px.x)),
      min(abs(px.y - min_px.y), abs(px.y - max_px.y)),
    );
    if (inside && edge_dist <= 1.5) {
      out = overlay_blend_over(out, vec4<f32>(0.0, 0.0, 0.0, 0.88));
    }
    if (inside && edge_dist <= 0.5) {
      out = overlay_blend_over(out, vec4<f32>(1.0, 1.0, 1.0, 0.97));
    }
  }
  return out;
}

fn overlay_layer_apply_motion(color: vec4<f32>, video_uv: vec2<f32>, track: i32) -> vec4<f32> {
  var out = color;
  let target_size = vec2<f32>(params.target_mode.x, params.target_mode.y);
  let px = video_uv * target_size;
  let count = max(0, params.overlay_counts.z);
  for (var i = 0; i < count; i = i + 1) {
    let rect = overlay_rects[u32(params.overlay_counts.x + params.overlay_counts.y + i)];
    if (!overlay_rect_matches_track(rect, track)) {
      continue;
    }
    let a = overlay_unpack_uv16(rect.rect_uv0) * target_size;
    let b = overlay_unpack_uv16(rect.rect_uv1) * target_size;
    if (overlay_segment_distance_px(px, a, b) <= 1.0) {
      out = overlay_blend_over(out, overlay_color_from_bgra(rect.color_bgra));
    }
  }
  return out;
}

fn overlay_layer_color(video_uv: vec2<f32>, track: i32) -> vec4<f32> {
  return overlay_layer_apply_motion(
      overlay_layer_apply_lines(overlay_layer_apply_fill(vec4<f32>(0.0), video_uv, track),
                                video_uv,
                                track),
      video_uv,
      track);
}

@fragment
fn fs_overlay_layer(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  let target_size = vec2<f32>(params.target_mode.x, params.target_mode.y);
  let video_uv = position.xy / target_size;
  let track = clamp(i32(round(params.split.z)), 0, 3);
  if (vec4_get_i(params.present, track) == 0) {
    return vec4<f32>(0.0);
  }
  return overlay_layer_color(video_uv, track);
}

fn overlay_apply_fill(color: vec4<f32>, tex_uv: vec2<f32>) -> vec4<f32> {
  var out = color;
  let count = max(0, params.overlay_counts.x);
  for (var i = 0; i < count; i = i + 1) {
    let rect = overlay_rects[u32(i)];
    let track = overlay_track_index(rect);
    if (vec4_get_i(params.present, track) == 0) {
      continue;
    }
    let global_rect = overlay_global_rect(rect);
    if (global_rect.z <= global_rect.x || global_rect.w <= global_rect.y) {
      continue;
    }
    if (tex_uv.x >= global_rect.x && tex_uv.x <= global_rect.z &&
        tex_uv.y >= global_rect.y && tex_uv.y <= global_rect.w) {
      out = overlay_blend_over(out, overlay_color_from_bgra(rect.color_bgra));
    }
  }
  return out;
}

fn overlay_apply_lines(color: vec4<f32>, tex_uv: vec2<f32>) -> vec4<f32> {
  var out = color;
  let target_size = vec2<f32>(params.target_mode.x, params.target_mode.y);
  let px = tex_uv * target_size;
  let count = max(0, params.overlay_counts.y);
  for (var i = 0; i < count; i = i + 1) {
    let rect = overlay_rects[u32(params.overlay_counts.x + i)];
    if (overlay_line_strength(rect) <= 0.0) {
      continue;
    }
    let track = overlay_track_index(rect);
    if (vec4_get_i(params.present, track) == 0) {
      continue;
    }
    let global_rect = overlay_global_rect(rect);
    if (global_rect.z <= global_rect.x || global_rect.w <= global_rect.y) {
      continue;
    }
    let min_px = global_rect.xy * target_size;
    let max_px = global_rect.zw * target_size;
    let inside = px.x >= min_px.x && px.x <= max_px.x &&
        px.y >= min_px.y && px.y <= max_px.y;
    let edge_dist = min(
      min(abs(px.x - min_px.x), abs(px.x - max_px.x)),
      min(abs(px.y - min_px.y), abs(px.y - max_px.y)),
    );
    if (inside && edge_dist <= 1.5) {
      out = overlay_blend_over(out, vec4<f32>(0.0, 0.0, 0.0, 0.88));
    }
    if (inside && edge_dist <= 0.5) {
      out = overlay_blend_over(out, vec4<f32>(1.0, 1.0, 1.0, 0.97));
    }
  }
  return out;
}

fn overlay_segment_distance_px(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>) -> f32 {
  let ab = b - a;
  let denom = max(dot(ab, ab), 0.00001);
  let t = clamp(dot(p - a, ab) / denom, 0.0, 1.0);
  return length(p - (a + ab * t));
}

fn overlay_apply_motion(color: vec4<f32>, tex_uv: vec2<f32>) -> vec4<f32> {
  var out = color;
  let target_size = vec2<f32>(params.target_mode.x, params.target_mode.y);
  let px = tex_uv * target_size;
  let count = max(0, params.overlay_counts.z);
  for (var i = 0; i < count; i = i + 1) {
    let rect = overlay_rects[u32(params.overlay_counts.x + params.overlay_counts.y + i)];
    let track = overlay_track_index(rect);
    if (vec4_get_i(params.present, track) == 0) {
      continue;
    }
    let local_a = overlay_local_from_video(overlay_unpack_uv16(rect.rect_uv0), track);
    let local_b = overlay_local_from_video(overlay_unpack_uv16(rect.rect_uv1), track);
    let visible = overlay_visible_local_rect_for_track(track);
    if ((local_a.x < visible.x && local_b.x < visible.x) ||
        (local_a.x > visible.z && local_b.x > visible.z) ||
        (local_a.y < visible.y && local_b.y < visible.y) ||
        (local_a.y > visible.w && local_b.y > visible.w)) {
      continue;
    }
    let a = overlay_global_from_local(local_a, track) * target_size;
    let b = overlay_global_from_local(local_b, track) * target_size;
    if (overlay_segment_distance_px(px, a, b) <= 1.0) {
      out = overlay_blend_over(out, overlay_color_from_bgra(rect.color_bgra));
    }
  }
  return out;
}

fn apply_overlay(color: vec4<f32>, tex_uv: vec2<f32>) -> vec4<f32> {
  return overlay_apply_motion(
      overlay_apply_lines(overlay_apply_fill(color, tex_uv), tex_uv), tex_uv);
}

fn read_u8(byte_offset: u32) -> u32 {
  let word = package_words[byte_offset / 4u];
  let shift = (byte_offset & 3u) * 8u;
  return (word >> shift) & 0xffu;
}

fn read_u16_le(byte_offset: u32) -> u32 {
  return read_u8(byte_offset) | (read_u8(byte_offset + 1u) << 8u);
}

fn sample_code(byte_offset: u32, high_bit: bool) -> f32 {
  if (high_bit) {
    return f32(read_u16_le(byte_offset) >> 6u);
  }
  return f32(read_u8(byte_offset));
}

fn matrix_rgb(y: f32, cb: f32, cr: f32, matrix: i32) -> vec3<f32> {
  if (matrix == 1) {
    return vec3<f32>(
      y + 1.402 * cr,
      y - 0.344136 * cb - 0.714136 * cr,
      y + 1.772 * cb,
    );
  }
  if (matrix == 3) {
    return vec3<f32>(
      y + 1.4746 * cr,
      y - 0.16455 * cb - 0.57135 * cr,
      y + 1.8814 * cb,
    );
  }
  return vec3<f32>(
    y + 1.5748 * cr,
    y - 0.1873 * cb - 0.4681 * cr,
    y + 1.8556 * cb,
  );
}

fn sample_yuv(track: i32, uv: vec2<f32>) -> vec4<f32> {
  let format = vec4_get_i(params.yuv_format, track);
  let high_bit = format == 2;
  let bytes_per_sample = select(1u, 2u, high_bit);
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let sx = clamp(i32(uv.x * f32(source_w)), 0, source_w - 1);
  let sy = clamp(i32(uv.y * f32(source_h)), 0, source_h - 1);
  let chroma_x = max(0, sx / 2);
  let chroma_y = max(0, sy / 2);
  let y_index = u32(vec4_get_i(params.y_offset, track) +
      sy * vec4_get_i(params.y_stride, track) +
      sx * i32(bytes_per_sample));
  let y_code = sample_code(y_index, high_bit);
  var u_code = 128.0;
  var v_code = 128.0;
  if (format == 3) {
    let u_index = u32(vec4_get_i(params.uv_offset, track) +
        chroma_y * vec4_get_i(params.uv_stride, track) + chroma_x);
    let v_index = u32(vec4_get_i(params.v_offset, track) +
        chroma_y * vec4_get_i(params.uv_stride, track) + chroma_x);
    u_code = sample_code(u_index, false);
    v_code = sample_code(v_index, false);
  } else {
    let pair_bytes = bytes_per_sample * 2u;
    let uv_index = u32(vec4_get_i(params.uv_offset, track) +
        chroma_y * vec4_get_i(params.uv_stride, track)) +
        u32(chroma_x) * pair_bytes;
    u_code = sample_code(uv_index, high_bit);
    v_code = sample_code(uv_index + bytes_per_sample, high_bit);
  }
  let range = vec4_get_i(params.color_range, track);
  let matrix = vec4_get_i(params.color_matrix, track);
  let scale = select(1.0, 4.0, high_bit);
  let max_code = select(255.0, 1023.0, high_bit);
  var y = y_code / max_code;
  var cb = u_code / max_code - 0.5;
  var cr = v_code / max_code - 0.5;
  if (range != 2) {
    y = clamp((y_code - 16.0 * scale) / (219.0 * scale), 0.0, 1.0);
    cb = (u_code - 128.0 * scale) / (224.0 * scale);
    cr = (v_code - 128.0 * scale) / (224.0 * scale);
  }
  return vec4<f32>(clamp(matrix_rgb(y, cb, cr, matrix), vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}

fn cv_yuv_to_rgb(track: i32, y_norm: f32, uv_norm: vec2<f32>) -> vec4<f32> {
  let format = vec4_get_i(params.yuv_format, track);
  let high_bit = format == 2;
  let range = vec4_get_i(params.color_range, track);
  let matrix = vec4_get_i(params.color_matrix, track);
  let scale = select(1.0, 4.0, high_bit);
  let max_code = select(255.0, 1023.0, high_bit);
  let y_code = y_norm * max_code;
  let u_code = uv_norm.x * max_code;
  let v_code = uv_norm.y * max_code;
  var y = y_code / max_code;
  var cb = u_code / max_code - 0.5;
  var cr = v_code / max_code - 0.5;
  if (range != 2) {
    y = clamp((y_code - 16.0 * scale) / (219.0 * scale), 0.0, 1.0);
    cb = (u_code - 128.0 * scale) / (224.0 * scale);
    cr = (v_code - 128.0 * scale) / (224.0 * scale);
  }
  return vec4<f32>(clamp(matrix_rgb(y, cb, cr, matrix), vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}

fn sample_cv_yuv_track(
    y_texture: texture_2d<f32>,
    uv_texture: texture_2d<f32>,
    track: i32,
    uv: vec2<f32>) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let coded_w = max(1, vec4_get_i(params.coded_width, track));
  let coded_h = max(1, vec4_get_i(params.coded_height, track));
  let sx = clamp(i32(uv.x * f32(source_w)), 0, source_w - 1);
  let sy = clamp(i32(uv.y * f32(source_h)), 0, source_h - 1);
  let y_x = min(sx, coded_w - 1);
  let y_y = min(sy, coded_h - 1);
  let uv_x = min(max(0, y_x / 2), max(1, (coded_w + 1) / 2) - 1);
  let uv_y = min(max(0, y_y / 2), max(1, (coded_h + 1) / 2) - 1);
  let y_norm = textureLoad(y_texture, vec2<i32>(y_x, y_y), 0).r;
  let uv_norm = textureLoad(uv_texture, vec2<i32>(uv_x, uv_y), 0).rg;
  return cv_yuv_to_rgb(track, y_norm, uv_norm);
}

fn sample_cv_yuv(track: i32, uv: vec2<f32>) -> vec4<f32> {
  if (track == 0) {
    return sample_cv_yuv_track(cv_y0, cv_uv0, track, uv);
  }
  if (track == 1) {
    return sample_cv_yuv_track(cv_y1, cv_uv1, track, uv);
  }
  if (track == 2) {
    return sample_cv_yuv_track(cv_y2, cv_uv2, track, uv);
  }
  return sample_cv_yuv_track(cv_y3, cv_uv3, track, uv);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  let target_size = vec2<f32>(params.target_mode.x, params.target_mode.y);
  let tex_uv = position.xy / target_size;
  let selection = select_track(tex_uv);
  let track = clamp(selection.x, 0, 3);
  let order_index = selection.y;
  if (vec4_get_i(params.present, track) == 0) {
    return map_sdr_ui_to_output(params.background);
  }
  let uv = source_uv(track, track_local_uv(tex_uv, order_index));
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    return map_sdr_ui_to_output(params.background);
  }
  let storage = i32(round(params.split.y));
  var color = textureSample(src_texture, src_sampler, uv, track);
  if (storage == 1) {
    color = sample_yuv(track, uv);
  } else if (storage == 3) {
    color = sample_cv_yuv(track, uv);
  }
  let divided = apply_split_divider(color, position.x);
  let output_color = map_source_to_output(divided, track);
  let overlay = map_sdr_ui_to_output(textureSample(overlay_layer_texture, src_sampler, uv, track));
  return overlay_blend_over(output_color, overlay);
}
