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
  output_size: vec4<f32>,
  viewport_rect: vec4<f32>,
  flutter_size: vec4<f32>,
  sdr_white: vec4<f32>,
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

@group(0) @binding(14)
var flutter_surface_texture: texture_2d<f32>;

struct VertexOut {
  @builtin(position) position: vec4<f32>,
};

struct OverlayFillVertexOut {
  @builtin(position) position: vec4<f32>,
  @location(0) color: vec4<f32>,
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
const STORAGE_YUV: i32 = 1;
const STORAGE_BGRA: i32 = 2;
const STORAGE_CV_PIXEL_BUFFER: i32 = 3;
const STORAGE_OUTPUT_ATLAS: i32 = 4;
const COLOR_PRIMARIES_BT601: i32 = 1;
const COLOR_PRIMARIES_BT2020: i32 = 3;
const OUTPUT_COLOR_MODE_MACOS_EDR: i32 = 2;
const OUTPUT_COLOR_MODE_WINDOWS_SCRGB: i32 = 3;
const HDR_REFERENCE_WHITE_NITS: f32 = 203.0;
const WINDOWS_SCRGB_REFERENCE_WHITE_NITS: f32 = 80.0;
const HLG_EDR_HEADROOM_SCALE: f32 = 4.0;

fn output_is_hdr() -> bool {
  return params.output_mode.x == OUTPUT_COLOR_MODE_MACOS_EDR ||
      params.output_mode.x == OUTPUT_COLOR_MODE_WINDOWS_SCRGB;
}

fn output_is_macos_edr() -> bool {
  return params.output_mode.x == OUTPUT_COLOR_MODE_MACOS_EDR;
}

fn output_is_windows_scrgb() -> bool {
  return params.output_mode.x == OUTPUT_COLOR_MODE_WINDOWS_SCRGB;
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

fn map_to_macos_edr(rgb: vec3<f32>, transfer: i32, primaries: i32) -> vec3<f32> {
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

fn map_to_windows_scrgb(rgb: vec3<f32>, transfer: i32, primaries: i32) -> vec3<f32> {
  if (transfer == COLOR_TRANSFER_PQ) {
    return max(
        convert_linear_primaries_to_bt709(
            pq_to_linear_nits(rgb) / WINDOWS_SCRGB_REFERENCE_WHITE_NITS,
            primaries),
        vec3<f32>(0.0));
  }
  if (transfer == COLOR_TRANSFER_HLG) {
    return max(
        convert_linear_primaries_to_bt709(
            hlg_to_linear(rgb) *
                (HLG_EDR_HEADROOM_SCALE * HDR_REFERENCE_WHITE_NITS /
                 WINDOWS_SCRGB_REFERENCE_WHITE_NITS),
            primaries),
        vec3<f32>(0.0));
  }
  let white_scale = max(params.sdr_white.x, 0.0001);
  return max(
      convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries) *
          white_scale,
      vec3<f32>(0.0));
}

fn map_source_to_output(color: vec4<f32>, track: i32) -> vec4<f32> {
  let transfer = color_transfer_at(track);
  let primaries = color_primaries_at(track);
  if (output_is_macos_edr()) {
    return vec4<f32>(map_to_macos_edr(color.rgb, transfer, primaries), color.a);
  }
  if (output_is_windows_scrgb()) {
    return vec4<f32>(map_to_windows_scrgb(color.rgb, transfer, primaries), color.a);
  }
  return vec4<f32>(tone_map_to_sdr(color.rgb, transfer, primaries), color.a);
}

fn map_sdr_ui_to_output(color: vec4<f32>) -> vec4<f32> {
  let ui = clamp(color, vec4<f32>(0.0), vec4<f32>(1.0));
  if (!output_is_hdr()) {
    return ui;
  }
  let white_scale = max(params.sdr_white.x, 0.0001);
  if (output_is_windows_scrgb()) {
    return vec4<f32>(srgb_to_linear(ui.rgb) * white_scale, ui.a);
  }
  return vec4<f32>(
      convert_linear_bt709_to_display_p3(srgb_to_linear(ui.rgb)) * white_scale,
      ui.a);
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

fn source_pixel_footprint(track: i32) -> f32 {
  let mode = i32(round(params.target_mode.z));
  let target_w = max(1.0, params.viewport_rect.z);
  let target_h = max(1.0, params.viewport_rect.w);
  let track_count = max(1, min(i32(round(params.target_mode.w)), 4));
  let local_step_x = select(
    f32(track_count) / target_w,
    1.0 / target_w,
    mode == 1);
  let local_step_y = 1.0 / target_h;
  let source_w = max(1.0, vec4_get_f(params.source_width, track));
  let source_h = max(1.0, vec4_get_f(params.source_height, track));
  let sample_step_x =
      source_w * local_step_x * abs(vec4_get_f(params.inv_display_size_x, track));
  let sample_step_y =
      source_h * local_step_y * abs(vec4_get_f(params.inv_display_size_y, track));
  return max(sample_step_x, sample_step_y);
}

fn should_bilinear_downsample(track: i32) -> bool {
  return source_pixel_footprint(track) > 1.0001;
}

fn apply_split_divider(color: vec4<f32>, tex_x: f32) -> vec4<f32> {
  let mode = i32(round(params.target_mode.z));
  if (mode != 1) {
    return color;
  }
  let divider_x = clamp(params.split.x, 0.0, 1.0) * max(1.0, params.viewport_rect.z);
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

fn overlay_unpack_uv16(packed: u32) -> vec2<f32> {
  return vec2<f32>(
    f32(packed & 0xffffu) / 65535.0,
    f32((packed >> 16u) & 0xffffu) / 65535.0,
  );
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

fn premul_blend_over(dst: vec4<f32>, src: vec4<f32>) -> vec4<f32> {
  let alpha = clamp(src.a, 0.0, 1.0);
  return vec4<f32>(
    src.rgb + dst.rgb * (1.0 - alpha),
    alpha + dst.a * (1.0 - alpha),
  );
}

fn map_premul_sdr_ui_to_output(color: vec4<f32>) -> vec4<f32> {
  let premul = clamp(color, vec4<f32>(0.0), vec4<f32>(1.0));
  if (!output_is_hdr()) {
    return premul;
  }
  if (premul.a <= 0.00001) {
    return vec4<f32>(0.0);
  }
  let straight = vec4<f32>(premul.rgb / premul.a, premul.a);
  let mapped = map_sdr_ui_to_output(straight);
  return vec4<f32>(mapped.rgb * premul.a, premul.a);
}

fn overlay_rect_matches_track(rect: OverlayRect, track: i32) -> bool {
  return overlay_track_index(rect) == track;
}

fn overlay_layer_rect(rect: OverlayRect) -> vec4<f32> {
  let a = overlay_unpack_uv16(rect.rect_uv0);
  let b = overlay_unpack_uv16(rect.rect_uv1);
  return vec4<f32>(min(a, b), max(a, b));
}

fn overlay_layer_size() -> vec2<f32> {
  return vec2<f32>(
    max(1.0, f32(params.output_mode.y)),
    max(1.0, f32(params.output_mode.z)),
  );
}

fn overlay_position_from_px(px: vec2<f32>, layer_size: vec2<f32>) -> vec4<f32> {
  return vec4<f32>(
    px.x / layer_size.x * 2.0 - 1.0,
    1.0 - px.y / layer_size.y * 2.0,
    0.0,
    1.0,
  );
}

fn display_slot_for_overlay_track(track: i32) -> i32 {
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

fn overlay_local_rect_from_video_rect(rect_min: vec2<f32>, rect_max: vec2<f32>, track: i32) -> vec4<f32> {
  let display_offset = vec2<f32>(
    vec4_get_f(params.display_offset_x, track),
    vec4_get_f(params.display_offset_y, track));
  let inv_display_size = vec2<f32>(
    vec4_get_f(params.inv_display_size_x, track),
    vec4_get_f(params.inv_display_size_y, track));
  let view_offset = vec2<f32>(
    vec4_get_f(params.view_offset_uv_x, track),
    vec4_get_f(params.view_offset_uv_y, track));
  let display_size = vec2<f32>(
    select(0.0, 1.0 / inv_display_size.x, abs(inv_display_size.x) > 0.00001),
    select(0.0, 1.0 / inv_display_size.y, abs(inv_display_size.y) > 0.00001));
  let local_min = display_offset + (rect_min + view_offset) * display_size;
  let local_max = display_offset + (rect_max + view_offset) * display_size;
  return vec4<f32>(min(local_min, local_max), max(local_min, local_max));
}

fn overlay_visible_local_rect(track: i32) -> vec4<f32> {
  var visible_min = vec2<f32>(0.0);
  var visible_max = vec2<f32>(1.0);
  if (i32(round(params.target_mode.z)) == 1) {
    let slot = display_slot_for_overlay_track(track);
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

fn overlay_global_uv_from_local_uv(local_uv: vec2<f32>, track: i32) -> vec2<f32> {
  if (i32(round(params.target_mode.z)) == 1) {
    return local_uv;
  }
  let track_count = max(1, min(i32(round(params.target_mode.w)), 4));
  let slot = clamp(display_slot_for_overlay_track(track), 0, track_count - 1);
  return vec2<f32>((f32(slot) + local_uv.x) / f32(track_count), local_uv.y);
}

fn overlay_viewport_position_from_px(px: vec2<f32>) -> vec4<f32> {
  let output_size = max(params.output_size.xy, vec2<f32>(1.0));
  return vec4<f32>(
    px.x / output_size.x * 2.0 - 1.0,
    1.0 - px.y / output_size.y * 2.0,
    0.0,
    1.0,
  );
}

fn overlay_fill_rect_vertex(vertex_index: u32) -> OverlayFillVertexOut {
  var out: OverlayFillVertexOut;
  out.position = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
  out.color = vec4<f32>(0.0);

  let layer_size = overlay_layer_size();
  let rect_id = vertex_index / 6u;
  let corner = vertex_index - rect_id * 6u;
  let rect = overlay_rects[rect_id];
  let track = clamp(i32(round(params.split.z)), 0, 3);
  if (!overlay_rect_matches_track(rect, track)) {
    return out;
  }

  let color = overlay_color_from_bgra(rect.color_bgra);
  if (color.a <= 0.0) {
    return out;
  }

  let r = overlay_layer_rect(rect);
  let min_px = clamp(floor(r.xy * layer_size), vec2<f32>(0.0), layer_size);
  let max_px = clamp(ceil(r.zw * layer_size), vec2<f32>(0.0), layer_size);
  if (max_px.x <= min_px.x || max_px.y <= min_px.y) {
    return out;
  }

  var px = min_px;
  if (corner == 1u || corner == 3u || corner == 4u) {
    px.x = max_px.x;
  }
  if (corner == 2u || corner == 4u || corner == 5u) {
    px.y = max_px.y;
  }
  out.position = overlay_position_from_px(px, layer_size);
  out.color = color;
  return out;
}

fn overlay_viewport_fill_rect_vertex(vertex_index: u32) -> OverlayFillVertexOut {
  var out: OverlayFillVertexOut;
  out.position = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
  out.color = vec4<f32>(0.0);

  let rect_id = vertex_index / 6u;
  let corner = vertex_index - rect_id * 6u;
  if (rect_id >= u32(max(0, params.overlay_counts.x))) {
    return out;
  }
  let rect = overlay_rects[rect_id];
  let track = overlay_track_index(rect);
  let color = overlay_color_from_bgra(rect.color_bgra);
  if (color.a <= 0.0) {
    return out;
  }

  let source_rect = overlay_layer_rect(rect);
  let local_rect = overlay_local_rect_from_video_rect(source_rect.xy, source_rect.zw, track);
  let visible_rect = overlay_visible_local_rect(track);
  let clipped_min = max(local_rect.xy, visible_rect.xy);
  let clipped_max = min(local_rect.zw, visible_rect.zw);
  if (clipped_max.x <= clipped_min.x || clipped_max.y <= clipped_min.y) {
    return out;
  }

  var local_uv = clipped_min;
  if (corner == 1u || corner == 3u || corner == 4u) {
    local_uv.x = clipped_max.x;
  }
  if (corner == 2u || corner == 4u || corner == 5u) {
    local_uv.y = clipped_max.y;
  }
  let global_uv = overlay_global_uv_from_local_uv(local_uv, track);
  let px = params.viewport_rect.xy + global_uv * params.viewport_rect.zw;
  out.position = overlay_viewport_position_from_px(px);
  out.color = color;
  return out;
}

@fragment
fn fs_overlay_primitive(in: OverlayFillVertexOut) -> @location(0) vec4<f32> {
  return in.color;
}

@fragment
fn fs_overlay_viewport_primitive(in: OverlayFillVertexOut) -> @location(0) vec4<f32> {
  return map_sdr_ui_to_output(in.color);
}

fn overlay_line_rect_vertex(
    vertex_index: u32,
    thickness: f32,
    color: vec4<f32>) -> OverlayFillVertexOut {
  var out: OverlayFillVertexOut;
  out.position = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
  out.color = vec4<f32>(0.0);

  let line_count = max(0, params.overlay_counts.y);
  if (line_count <= 0) {
    return out;
  }
  let primitive_id = vertex_index / 6u;
  let rect_id = primitive_id / 4u;
  let side = primitive_id - rect_id * 4u;
  let corner = vertex_index - primitive_id * 6u;
  let rect = overlay_rects[u32(params.overlay_counts.x) + rect_id];
  let track = clamp(i32(round(params.split.z)), 0, 3);
  if (!overlay_rect_matches_track(rect, track) || overlay_line_strength(rect) <= 0.0) {
    return out;
  }

  let layer_size = overlay_layer_size();
  let r = overlay_layer_rect(rect);
  let min_px = clamp(floor(r.xy * layer_size), vec2<f32>(0.0), layer_size);
  let max_px = clamp(ceil(r.zw * layer_size), vec2<f32>(0.0), layer_size);
  if (max_px.x <= min_px.x || max_px.y <= min_px.y) {
    return out;
  }

  var strip_min = min_px;
  var strip_max = max_px;
  if (side == 0u) {
    strip_max.y = min(max_px.y, min_px.y + thickness);
  } else if (side == 1u) {
    strip_min.y = max(min_px.y, max_px.y - thickness);
  } else if (side == 2u) {
    strip_max.x = min(max_px.x, min_px.x + thickness);
  } else {
    strip_min.x = max(min_px.x, max_px.x - thickness);
  }

  var px = strip_min;
  if (corner == 1u || corner == 3u || corner == 4u) {
    px.x = strip_max.x;
  }
  if (corner == 2u || corner == 4u || corner == 5u) {
    px.y = strip_max.y;
  }
  out.position = overlay_position_from_px(px, layer_size);
  out.color = color;
  return out;
}

fn overlay_viewport_line_rect_vertex(
    vertex_index: u32,
    thickness: f32,
    color: vec4<f32>) -> OverlayFillVertexOut {
  var out: OverlayFillVertexOut;
  out.position = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
  out.color = vec4<f32>(0.0);

  let line_count = max(0, params.overlay_counts.y);
  if (line_count <= 0) {
    return out;
  }
  let primitive_id = vertex_index / 6u;
  let rect_id = primitive_id / 4u;
  let side = primitive_id - rect_id * 4u;
  let corner = vertex_index - primitive_id * 6u;
  if (rect_id >= u32(line_count)) {
    return out;
  }
  let rect = overlay_rects[u32(params.overlay_counts.x) + rect_id];
  let track = overlay_track_index(rect);
  if (overlay_line_strength(rect) <= 0.0) {
    return out;
  }

  let source_rect = overlay_layer_rect(rect);
  let local_rect = overlay_local_rect_from_video_rect(source_rect.xy, source_rect.zw, track);
  let visible_rect = overlay_visible_local_rect(track);
  let clipped_min = max(local_rect.xy, visible_rect.xy);
  let clipped_max = min(local_rect.zw, visible_rect.zw);
  if (clipped_max.x <= clipped_min.x || clipped_max.y <= clipped_min.y) {
    return out;
  }

  let global_min = overlay_global_uv_from_local_uv(clipped_min, track);
  let global_max = overlay_global_uv_from_local_uv(clipped_max, track);
  let rect_global_min = overlay_global_uv_from_local_uv(local_rect.xy, track);
  let rect_global_max = overlay_global_uv_from_local_uv(local_rect.zw, track);
  let visible_global_min = overlay_global_uv_from_local_uv(visible_rect.xy, track);
  let visible_global_max = overlay_global_uv_from_local_uv(visible_rect.zw, track);
  let viewport_min = params.viewport_rect.xy;
  let viewport_size = params.viewport_rect.zw;
  let clipped_px_min = viewport_min + min(global_min, global_max) * viewport_size;
  let clipped_px_max = viewport_min + max(global_min, global_max) * viewport_size;
  let visible_px_min = viewport_min + min(visible_global_min, visible_global_max) * viewport_size;
  let visible_px_max = viewport_min + max(visible_global_min, visible_global_max) * viewport_size;
  let rect_min_px = viewport_min + min(rect_global_min, rect_global_max) * viewport_size;
  let rect_max_px = viewport_min + max(rect_global_min, rect_global_max) * viewport_size;
  let snapped_rect = vec4<f32>(
    floor(rect_min_px.x + 0.5),
    floor(rect_min_px.y + 0.5),
    floor(rect_max_px.x + 0.5),
    floor(rect_max_px.y + 0.5));

  let draw_min = max(clipped_px_min - vec2<f32>(thickness), visible_px_min);
  let draw_max = min(clipped_px_max + vec2<f32>(thickness), visible_px_max);
  var strip_min = draw_min;
  var strip_max = draw_max;
  if (side == 0u) {
    strip_min.y = snapped_rect.y;
    strip_max.y = min(draw_max.y, snapped_rect.y + thickness);
  } else if (side == 1u) {
    strip_min.y = max(draw_min.y, snapped_rect.w - thickness);
    strip_max.y = snapped_rect.w;
  } else if (side == 2u) {
    strip_min.x = snapped_rect.x;
    strip_max.x = min(draw_max.x, snapped_rect.x + thickness);
  } else {
    strip_min.x = max(draw_min.x, snapped_rect.z - thickness);
    strip_max.x = snapped_rect.z;
  }
  if (strip_max.x <= strip_min.x || strip_max.y <= strip_min.y) {
    return out;
  }

  var px = strip_min;
  if (corner == 1u || corner == 3u || corner == 4u) {
    px.x = strip_max.x;
  }
  if (corner == 2u || corner == 4u || corner == 5u) {
    px.y = strip_max.y;
  }
  out.position = overlay_viewport_position_from_px(px);
  out.color = color;
  return out;
}

fn overlay_motion_line_vertex(vertex_index: u32) -> OverlayFillVertexOut {
  var out: OverlayFillVertexOut;
  out.position = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
  out.color = vec4<f32>(0.0);

  let motion_count = max(0, params.overlay_counts.z);
  if (motion_count <= 0) {
    return out;
  }
  let line_id = vertex_index / 6u;
  let corner = vertex_index - line_id * 6u;
  let rect = overlay_rects[u32(params.overlay_counts.x + params.overlay_counts.y) + line_id];
  let track = clamp(i32(round(params.split.z)), 0, 3);
  if (!overlay_rect_matches_track(rect, track)) {
    return out;
  }

  let color = overlay_color_from_bgra(rect.color_bgra);
  if (color.a <= 0.0) {
    return out;
  }

  let layer_size = overlay_layer_size();
  let a = overlay_unpack_uv16(rect.rect_uv0) * layer_size;
  let b = overlay_unpack_uv16(rect.rect_uv1) * layer_size;
  let delta = b - a;
  let len = length(delta);
  if (len <= 0.001) {
    return out;
  }
  let normal = vec2<f32>(-delta.y, delta.x) / len;
  let half_width = 1.0;

  var px = a - normal * half_width;
  if (corner == 1u || corner == 3u) {
    px = b - normal * half_width;
  } else if (corner == 2u || corner == 5u) {
    px = a + normal * half_width;
  } else if (corner == 4u) {
    px = b + normal * half_width;
  }
  out.position = overlay_position_from_px(clamp(px, vec2<f32>(0.0), layer_size), layer_size);
  out.color = color;
  return out;
}

fn overlay_viewport_motion_line_vertex(vertex_index: u32) -> OverlayFillVertexOut {
  var out: OverlayFillVertexOut;
  out.position = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
  out.color = vec4<f32>(0.0);

  let motion_count = max(0, params.overlay_counts.z);
  if (motion_count <= 0) {
    return out;
  }
  let line_id = vertex_index / 6u;
  let corner = vertex_index - line_id * 6u;
  if (line_id >= u32(motion_count)) {
    return out;
  }
  let rect = overlay_rects[u32(params.overlay_counts.x + params.overlay_counts.y) + line_id];
  let track = overlay_track_index(rect);
  let color = overlay_color_from_bgra(rect.color_bgra);
  if (color.a <= 0.0) {
    return out;
  }

  let a_uv = overlay_unpack_uv16(rect.rect_uv0);
  let b_uv = overlay_unpack_uv16(rect.rect_uv1);
  let local_a = overlay_local_rect_from_video_rect(a_uv, a_uv, track).xy;
  let local_b = overlay_local_rect_from_video_rect(b_uv, b_uv, track).xy;
  let visible_rect = overlay_visible_local_rect(track);
  if (local_a.x < visible_rect.x || local_a.x > visible_rect.z ||
      local_a.y < visible_rect.y || local_a.y > visible_rect.w ||
      local_b.x < visible_rect.x || local_b.x > visible_rect.z ||
      local_b.y < visible_rect.y || local_b.y > visible_rect.w) {
    return out;
  }

  let a = params.viewport_rect.xy +
      overlay_global_uv_from_local_uv(local_a, track) * params.viewport_rect.zw;
  let b = params.viewport_rect.xy +
      overlay_global_uv_from_local_uv(local_b, track) * params.viewport_rect.zw;
  let delta = b - a;
  let len = length(delta);
  if (len <= 0.001) {
    return out;
  }
  let normal = vec2<f32>(-delta.y, delta.x) / len;
  let half_width = 1.0;

  var px = a - normal * half_width;
  if (corner == 1u || corner == 3u) {
    px = b - normal * half_width;
  } else if (corner == 2u || corner == 5u) {
    px = a + normal * half_width;
  } else if (corner == 4u) {
    px = b + normal * half_width;
  }
  out.position = overlay_viewport_position_from_px(px);
  out.color = color;
  return out;
}

@vertex
fn vs_overlay_primitive(@builtin(vertex_index) vertex_index: u32) -> OverlayFillVertexOut {
  let fill_vertices = u32(max(0, params.overlay_counts.x)) * 6u;
  if (vertex_index < fill_vertices) {
    return overlay_fill_rect_vertex(vertex_index);
  }

  let after_fill = vertex_index - fill_vertices;
  let line_vertices = u32(max(0, params.overlay_counts.y)) * 4u * 6u;
  if (after_fill < line_vertices) {
    return overlay_line_rect_vertex(after_fill, 2.0, vec4<f32>(0.0, 0.0, 0.0, 0.88));
  }

  let after_line_black = after_fill - line_vertices;
  if (after_line_black < line_vertices) {
    return overlay_line_rect_vertex(after_line_black, 1.0, vec4<f32>(1.0, 1.0, 1.0, 0.97));
  }

  return overlay_motion_line_vertex(after_line_black - line_vertices);
}

@vertex
fn vs_overlay_viewport_primitive(@builtin(vertex_index) vertex_index: u32) -> OverlayFillVertexOut {
  let fill_vertices = u32(max(0, params.overlay_counts.x)) * 6u;
  if (vertex_index < fill_vertices) {
    return overlay_viewport_fill_rect_vertex(vertex_index);
  }

  let after_fill = vertex_index - fill_vertices;
  let line_vertices = u32(max(0, params.overlay_counts.y)) * 4u * 6u;
  if (after_fill < line_vertices) {
    return overlay_viewport_line_rect_vertex(after_fill, 2.0, vec4<f32>(0.0, 0.0, 0.0, 0.88));
  }

  let after_line_black = after_fill - line_vertices;
  if (after_line_black < line_vertices) {
    return overlay_viewport_line_rect_vertex(after_line_black, 1.0, vec4<f32>(1.0, 1.0, 1.0, 0.97));
  }

  return overlay_viewport_motion_line_vertex(after_line_black - line_vertices);
}

@fragment
fn fs_flutter_surface(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  let flutter_size = max(params.flutter_size.xy, vec2<f32>(1.0));
  if (position.x < 0.0 || position.y < 0.0 ||
      position.x >= flutter_size.x || position.y >= flutter_size.y) {
    return vec4<f32>(0.0);
  }
  let sample = textureSample(flutter_surface_texture, src_sampler, position.xy / flutter_size);
  return map_premul_sdr_ui_to_output(sample);
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

fn sample_yuv_pixel(track: i32, sx: i32, sy: i32) -> vec4<f32> {
  let format = vec4_get_i(params.yuv_format, track);
  let high_bit = format == 2;
  let bytes_per_sample = select(1u, 2u, high_bit);
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let clamped_x = clamp(sx, 0, source_w - 1);
  let clamped_y = clamp(sy, 0, source_h - 1);
  let chroma_x = max(0, clamped_x / 2);
  let chroma_y = max(0, clamped_y / 2);
  let y_index = u32(vec4_get_i(params.y_offset, track) +
      clamped_y * vec4_get_i(params.y_stride, track) +
      clamped_x * i32(bytes_per_sample));
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

fn sample_yuv_nearest(track: i32, uv: vec2<f32>) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let sx = clamp(i32(uv.x * f32(source_w)), 0, source_w - 1);
  let sy = clamp(i32(uv.y * f32(source_h)), 0, source_h - 1);
  return sample_yuv_pixel(track, sx, sy);
}

fn sample_yuv_bilinear(track: i32, uv: vec2<f32>) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let coord = uv * vec2<f32>(f32(source_w), f32(source_h)) - vec2<f32>(0.5);
  let base_f = floor(coord);
  let base = vec2<i32>(i32(base_f.x), i32(base_f.y));
  let frac = clamp(coord - base_f, vec2<f32>(0.0), vec2<f32>(1.0));
  let c00 = sample_yuv_pixel(track, base.x, base.y);
  let c10 = sample_yuv_pixel(track, base.x + 1, base.y);
  let c01 = sample_yuv_pixel(track, base.x, base.y + 1);
  let c11 = sample_yuv_pixel(track, base.x + 1, base.y + 1);
  return mix(mix(c00, c10, frac.x), mix(c01, c11, frac.x), frac.y);
}

fn sample_yuv(track: i32, uv: vec2<f32>) -> vec4<f32> {
  if (should_bilinear_downsample(track)) {
    return sample_yuv_bilinear(track, uv);
  }
  return sample_yuv_nearest(track, uv);
}

fn sample_bgra_pixel(track: i32, sx: i32, sy: i32) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  return textureLoad(
    src_texture,
    vec2<i32>(clamp(sx, 0, source_w - 1), clamp(sy, 0, source_h - 1)),
    track,
    0);
}

fn sample_bgra_nearest(track: i32, uv: vec2<f32>) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let sx = clamp(i32(uv.x * f32(source_w)), 0, source_w - 1);
  let sy = clamp(i32(uv.y * f32(source_h)), 0, source_h - 1);
  return sample_bgra_pixel(track, sx, sy);
}

fn sample_bgra_bilinear(track: i32, uv: vec2<f32>) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let coord = uv * vec2<f32>(f32(source_w), f32(source_h)) - vec2<f32>(0.5);
  let base_f = floor(coord);
  let base = vec2<i32>(i32(base_f.x), i32(base_f.y));
  let frac = clamp(coord - base_f, vec2<f32>(0.0), vec2<f32>(1.0));
  let c00 = sample_bgra_pixel(track, base.x, base.y);
  let c10 = sample_bgra_pixel(track, base.x + 1, base.y);
  let c01 = sample_bgra_pixel(track, base.x, base.y + 1);
  let c11 = sample_bgra_pixel(track, base.x + 1, base.y + 1);
  return mix(mix(c00, c10, frac.x), mix(c01, c11, frac.x), frac.y);
}

fn sample_bgra(track: i32, uv: vec2<f32>) -> vec4<f32> {
  if (should_bilinear_downsample(track)) {
    return sample_bgra_bilinear(track, uv);
  }
  return sample_bgra_nearest(track, uv);
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
    sx: i32,
    sy: i32) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let coded_w = max(1, vec4_get_i(params.coded_width, track));
  let coded_h = max(1, vec4_get_i(params.coded_height, track));
  let y_x = min(clamp(sx, 0, source_w - 1), coded_w - 1);
  let y_y = min(clamp(sy, 0, source_h - 1), coded_h - 1);
  let uv_x = min(max(0, y_x / 2), max(1, (coded_w + 1) / 2) - 1);
  let uv_y = min(max(0, y_y / 2), max(1, (coded_h + 1) / 2) - 1);
  let y_norm = textureLoad(y_texture, vec2<i32>(y_x, y_y), 0).r;
  let uv_norm = textureLoad(uv_texture, vec2<i32>(uv_x, uv_y), 0).rg;
  return cv_yuv_to_rgb(track, y_norm, uv_norm);
}

fn sample_cv_yuv_track_nearest(
    y_texture: texture_2d<f32>,
    uv_texture: texture_2d<f32>,
    track: i32,
    uv: vec2<f32>) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let sx = clamp(i32(uv.x * f32(source_w)), 0, source_w - 1);
  let sy = clamp(i32(uv.y * f32(source_h)), 0, source_h - 1);
  return sample_cv_yuv_track(y_texture, uv_texture, track, sx, sy);
}

fn sample_cv_yuv_track_bilinear(
    y_texture: texture_2d<f32>,
    uv_texture: texture_2d<f32>,
    track: i32,
    uv: vec2<f32>) -> vec4<f32> {
  let source_w = max(1, i32(vec4_get_f(params.source_width, track)));
  let source_h = max(1, i32(vec4_get_f(params.source_height, track)));
  let coord = uv * vec2<f32>(f32(source_w), f32(source_h)) - vec2<f32>(0.5);
  let base_f = floor(coord);
  let base = vec2<i32>(i32(base_f.x), i32(base_f.y));
  let frac = clamp(coord - base_f, vec2<f32>(0.0), vec2<f32>(1.0));
  let c00 = sample_cv_yuv_track(y_texture, uv_texture, track, base.x, base.y);
  let c10 = sample_cv_yuv_track(y_texture, uv_texture, track, base.x + 1, base.y);
  let c01 = sample_cv_yuv_track(y_texture, uv_texture, track, base.x, base.y + 1);
  let c11 = sample_cv_yuv_track(y_texture, uv_texture, track, base.x + 1, base.y + 1);
  return mix(mix(c00, c10, frac.x), mix(c01, c11, frac.x), frac.y);
}

fn sample_cv_yuv(track: i32, uv: vec2<f32>) -> vec4<f32> {
  if (track == 0) {
    if (should_bilinear_downsample(track)) {
      return sample_cv_yuv_track_bilinear(cv_y0, cv_uv0, track, uv);
    }
    return sample_cv_yuv_track_nearest(cv_y0, cv_uv0, track, uv);
  }
  if (track == 1) {
    if (should_bilinear_downsample(track)) {
      return sample_cv_yuv_track_bilinear(cv_y1, cv_uv1, track, uv);
    }
    return sample_cv_yuv_track_nearest(cv_y1, cv_uv1, track, uv);
  }
  if (track == 2) {
    if (should_bilinear_downsample(track)) {
      return sample_cv_yuv_track_bilinear(cv_y2, cv_uv2, track, uv);
    }
    return sample_cv_yuv_track_nearest(cv_y2, cv_uv2, track, uv);
  }
  if (should_bilinear_downsample(track)) {
    return sample_cv_yuv_track_bilinear(cv_y3, cv_uv3, track, uv);
  }
  return sample_cv_yuv_track_nearest(cv_y3, cv_uv3, track, uv);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  let direct_viewport_overlay = params.output_mode.w == 1;
  let viewport_min = params.viewport_rect.xy;
  let viewport_size = max(params.viewport_rect.zw, vec2<f32>(1.0));
  let viewport_max = viewport_min + viewport_size;
  var with_overlay = map_sdr_ui_to_output(params.background);

  if (position.x >= viewport_min.x && position.y >= viewport_min.y &&
      position.x < viewport_max.x && position.y < viewport_max.y) {
    let viewport_uv = (position.xy - viewport_min) / viewport_size;
    let selection = select_track(viewport_uv);
    let track = clamp(selection.x, 0, 3);
    let order_index = selection.y;
    if (vec4_get_i(params.present, track) != 0) {
      let uv = source_uv(track, track_local_uv(viewport_uv, order_index));
      if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        let storage = i32(round(params.split.y));
        var color = sample_bgra(track, uv);
        if (storage == STORAGE_YUV) {
          color = sample_yuv(track, uv);
        } else if (storage == STORAGE_CV_PIXEL_BUFFER) {
          color = sample_cv_yuv(track, uv);
        }
        let divided = apply_split_divider(color, position.x - viewport_min.x);
        let output_color = select(
          map_source_to_output(divided, track),
          divided,
          storage == STORAGE_OUTPUT_ATLAS);
        if (direct_viewport_overlay) {
          with_overlay = output_color;
        } else {
          let overlay = map_sdr_ui_to_output(
            textureSample(overlay_layer_texture, src_sampler, uv, track));
          with_overlay = overlay_blend_over(output_color, overlay);
        }
      }
    }
  }

  return with_overlay;
}
