use crate::overlay::OverlayRect;
use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2_metal::{MTLPixelFormat, MTLResource, MTLTexture, MTLTextureType};
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::Instant;

use crate::MAX_TRACKS;

const STORAGE_NONE: i32 = 0;
const STORAGE_YUV: i32 = 1;
const STORAGE_BGRA: i32 = 2;
const STORAGE_CV_PIXEL_BUFFER: i32 = 3;
const STORAGE_OUTPUT_ATLAS: i32 = 4;
const OUTPUT_FORMAT_BGRA8_UNORM: i32 = 1;
const OUTPUT_FORMAT_RGBA16_FLOAT: i32 = 2;
const OUTPUT_COLOR_MODE_SDR: i32 = 1;
const OUTPUT_COLOR_MODE_MACOS_EDR: i32 = 2;

fn profile_elapsed_us(start: Instant) -> u64 {
    start.elapsed().as_micros().min(u128::from(u64::MAX)) as u64
}

#[repr(C)]
pub struct WgpuMetalRenderRequest {
    pub destination_mtl_texture: *mut core::ffi::c_void,
    pub output_format: i32,
    pub output_color_mode: i32,
    pub sdr_white_scale: f32,
    pub package_data: *const u8,
    pub package_data_size: usize,
    pub package: *const core::ffi::c_void,
    pub overlay_fill_rects: *const OverlayRect,
    pub overlay_fill_rect_count: usize,
    pub overlay_line_rects: *const OverlayRect,
    pub overlay_line_rect_count: usize,
    pub overlay_motion_lines: *const OverlayRect,
    pub overlay_motion_line_count: usize,
    pub overlay_generation: u64,
    pub width: i32,
    pub height: i32,
    pub viewport_left: f32,
    pub viewport_top: f32,
    pub viewport_right: f32,
    pub viewport_bottom: f32,
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
    pub flutter_mtl_texture: *mut core::ffi::c_void,
    pub flutter_width: i32,
    pub flutter_height: i32,
}

#[repr(C)]
pub struct WgpuMetalCVPixelBufferRenderRequest {
    pub destination_mtl_texture: *mut core::ffi::c_void,
    pub output_format: i32,
    pub output_color_mode: i32,
    pub sdr_white_scale: f32,
    pub source_y_mtl_textures: [*mut core::ffi::c_void; MAX_TRACKS],
    pub source_uv_mtl_textures: [*mut core::ffi::c_void; MAX_TRACKS],
    pub frame_set: *const core::ffi::c_void,
    pub overlay_fill_rects: *const OverlayRect,
    pub overlay_fill_rect_count: usize,
    pub overlay_line_rects: *const OverlayRect,
    pub overlay_line_rect_count: usize,
    pub overlay_motion_lines: *const OverlayRect,
    pub overlay_motion_line_count: usize,
    pub overlay_generation: u64,
    pub width: i32,
    pub height: i32,
    pub viewport_left: f32,
    pub viewport_top: f32,
    pub viewport_right: f32,
    pub viewport_bottom: f32,
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
    pub flutter_mtl_texture: *mut core::ffi::c_void,
    pub flutter_width: i32,
    pub flutter_height: i32,
}

#[repr(C)]
pub struct WgpuMetalRetainedCompositeRequest {
    pub destination_mtl_texture: *mut core::ffi::c_void,
    pub output_format: i32,
    pub output_color_mode: i32,
    pub sdr_white_scale: f32,
    pub decision: *const core::ffi::c_void,
    pub overlay_fill_rects: *const OverlayRect,
    pub overlay_fill_rect_count: usize,
    pub overlay_line_rects: *const OverlayRect,
    pub overlay_line_rect_count: usize,
    pub overlay_motion_lines: *const OverlayRect,
    pub overlay_motion_line_count: usize,
    pub overlay_generation: u64,
    pub width: i32,
    pub height: i32,
    pub viewport_left: f32,
    pub viewport_top: f32,
    pub viewport_right: f32,
    pub viewport_bottom: f32,
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
    pub flutter_mtl_texture: *mut core::ffi::c_void,
    pub flutter_width: i32,
    pub flutter_height: i32,
}

struct CompletionJob {
    submission: wgpu::SubmissionIndex,
    callback: WgpuMetalAsyncCompletionCallback,
    user_data: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgpuMetalProfilerSnapshot {
    pub destination_import_count: u64,
    pub destination_import_reuse_count: u64,
    pub source_import_count: u64,
    pub source_import_reuse_count: u64,
    pub imported_texture_cache_size: u64,
    pub imported_texture_cache_eviction_count: u64,
    pub final_bind_group_create_count: u64,
    pub overlay_bind_group_create_count: u64,
    pub overlay_layer_rebuild_count: u64,
    pub overlay_layer_reuse_count: u64,
    pub package_buffer_write_count: u64,
    pub params_buffer_write_count: u64,
    pub overlay_buffer_write_count: u64,
    pub submit_count: u64,
    pub last_import_us: u64,
    pub last_prepare_us: u64,
    pub last_overlay_encode_us: u64,
    pub last_bind_group_us: u64,
    pub last_pass_encode_us: u64,
    pub last_submit_us: u64,
    pub last_cpu_render_us: u64,
}

pub type WgpuMetalAsyncCompletionCallback = extern "C" fn(*mut core::ffi::c_void, i32);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgpuMetalAsyncCompletion {
    pub callback: Option<WgpuMetalAsyncCompletionCallback>,
    pub user_data: *mut core::ffi::c_void,
    pub profiler_snapshot: *mut WgpuMetalProfilerSnapshot,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PresentFrameInfo {
    present: i32,
    file_id: i32,
    slot: i32,
    width: i32,
    height: i32,
    pts_us: i64,
    dts_us: i64,
    duration_us: i64,
    analysis_frame_index: i32,
    frame_identity_mode: i32,
    source_packet_index: i32,
    source_packet_size: i32,
    source_packet_pos: i64,
    source_packet_pts: i64,
    source_packet_dts: i64,
    color_range: i32,
    color_matrix: i32,
    color_transfer: i32,
    color_primaries: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct PresentDecisionInfo {
    should_present: i32,
    frame_count: i32,
    track_count: i32,
    mode: i32,
    current_pts_us: i64,
    split_pos: f32,
    background_color: [f32; 4],
    order: [i32; MAX_TRACKS],
    display_offset_x: [f32; MAX_TRACKS],
    display_offset_y: [f32; MAX_TRACKS],
    inv_display_size_x: [f32; MAX_TRACKS],
    inv_display_size_y: [f32; MAX_TRACKS],
    view_offset_uv_x: [f32; MAX_TRACKS],
    view_offset_uv_y: [f32; MAX_TRACKS],
    source_width: [i32; MAX_TRACKS],
    source_height: [i32; MAX_TRACKS],
    yuv_format: [i32; MAX_TRACKS],
    y_offset: [i32; MAX_TRACKS],
    uv_offset: [i32; MAX_TRACKS],
    v_offset: [i32; MAX_TRACKS],
    y_stride: [i32; MAX_TRACKS],
    uv_stride: [i32; MAX_TRACKS],
    coded_width: [i32; MAX_TRACKS],
    coded_height: [i32; MAX_TRACKS],
    nv12_uv_scale_x: [f32; MAX_TRACKS],
    nv12_uv_scale_y: [f32; MAX_TRACKS],
    color_range: [i32; MAX_TRACKS],
    color_matrix: [i32; MAX_TRACKS],
    color_transfer: [i32; MAX_TRACKS],
    color_primaries: [i32; MAX_TRACKS],
    frames: [PresentFrameInfo; MAX_TRACKS],
}

#[repr(C)]
struct PresentFramePackageInfo {
    storage: i32,
    width: i32,
    height: i32,
    max_track_slots: i32,
    stride_bytes: i32,
    track_stride_bytes: usize,
    used_bytes: usize,
    decision: PresentDecisionInfo,
}

#[repr(C)]
struct CVPixelBufferPresentFrameSet {
    pixel_buffers: [*mut core::ffi::c_void; MAX_TRACKS],
    pixel_formats: [i32; MAX_TRACKS],
    plane_counts: [i32; MAX_TRACKS],
    is_p010: [i32; MAX_TRACKS],
    coded_widths: [i32; MAX_TRACKS],
    coded_heights: [i32; MAX_TRACKS],
    decision: PresentDecisionInfo,
}

#[derive(Clone, Copy)]
struct OutputTarget {
    format: wgpu::TextureFormat,
    color_mode: i32,
    width: u32,
    height: u32,
}

fn output_target_from_request(
    output_format: i32,
    output_color_mode: i32,
    width: i32,
    height: i32,
) -> Result<OutputTarget, &'static str> {
    if width <= 0 || height <= 0 {
        return Err("wgpu-metal output target dimensions are invalid");
    }
    match (output_format, output_color_mode) {
        (OUTPUT_FORMAT_BGRA8_UNORM, OUTPUT_COLOR_MODE_SDR) => Ok(OutputTarget {
            format: wgpu::TextureFormat::Bgra8Unorm,
            color_mode: output_color_mode,
            width: width as u32,
            height: height as u32,
        }),
        (OUTPUT_FORMAT_RGBA16_FLOAT, OUTPUT_COLOR_MODE_MACOS_EDR) => Ok(OutputTarget {
            format: wgpu::TextureFormat::Rgba16Float,
            color_mode: output_color_mode,
            width: width as u32,
            height: height as u32,
        }),
        _ => Err("wgpu-metal output target format/color mode is unsupported"),
    }
}

pub fn render_metal_package(request: &WgpuMetalRenderRequest) -> Result<(), &'static str> {
    let mut renderer = WgpuMetalRenderer::new()?;
    render_metal_package_with_renderer(&mut renderer, request)
}

pub fn render_metal_package_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRenderRequest,
) -> Result<(), &'static str> {
    let submission = submit_metal_package_with_renderer(renderer, request)?;
    wait_for_submission(&renderer.device, submission, "wgpu-metal queue wait failed")
}

pub fn bake_metal_package_source_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRenderRequest,
) -> Result<(), &'static str> {
    let submission = submit_metal_package_source_with_renderer(renderer, request)?;
    wait_for_submission(
        &renderer.device,
        submission,
        "wgpu-metal package source bake wait failed",
    )
}

pub fn bake_metal_package_source_with_renderer_async(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRenderRequest,
    completion: WgpuMetalAsyncCompletion,
) -> Result<(), &'static str> {
    renderer.begin_profile_frame();
    let profile_start = Instant::now();
    let submission = match submit_metal_package_source_with_renderer(renderer, request) {
        Ok(submission) => submission,
        Err(error) => {
            renderer.profiler.last_cpu_render_us = profile_elapsed_us(profile_start);
            return Err(error);
        }
    };
    renderer.profiler.last_cpu_render_us = profile_elapsed_us(profile_start);
    renderer.submit_completion(submission, completion)
}

fn submit_metal_package_source_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRenderRequest,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if request.package.is_null() {
        return Err("wgpu-metal package metadata is null");
    }
    if request.package_data.is_null() || request.package_data_size == 0 {
        return Err("wgpu-metal package data is empty");
    }
    if request.width <= 0 || request.height <= 0 {
        return Err("wgpu-metal package source bake dimensions are invalid");
    }
    let output = output_target_from_request(
        request.output_format,
        request.output_color_mode,
        request.width,
        request.height,
    )?;
    let package = unsafe { &*(request.package.cast::<PresentFramePackageInfo>()) };
    if package.width <= 0 || package.height <= 0 {
        return Err("wgpu-metal package dimensions are invalid");
    }
    if package.storage != STORAGE_BGRA {
        return Err("wgpu-metal package source bake only supports BGRA source atlas");
    }
    let source =
        unsafe { core::slice::from_raw_parts(request.package_data, request.package_data_size) };
    bake_bgra_package_source_atlas(renderer, source, package, output, request.sdr_white_scale)
}

fn submit_metal_package_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRenderRequest,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if request.destination_mtl_texture.is_null() {
        return Err("wgpu-metal destination texture is null");
    }
    if request.package.is_null() {
        return Err("wgpu-metal package metadata is null");
    }
    if request.package_data.is_null() || request.package_data_size == 0 {
        return Err("wgpu-metal package data is empty");
    }
    let output = output_target_from_request(
        request.output_format,
        request.output_color_mode,
        request.width,
        request.height,
    )?;
    let package = unsafe { &*(request.package.cast::<PresentFramePackageInfo>()) };
    if package.width <= 0 || package.height <= 0 {
        return Err("wgpu-metal package dimensions are invalid");
    }
    let viewport_rect = composite_viewport_rect(
        request.width,
        request.height,
        request.viewport_left,
        request.viewport_top,
        request.viewport_right,
        request.viewport_bottom,
    );
    if package.storage != STORAGE_BGRA && package.storage != STORAGE_YUV {
        return Err("wgpu-metal package storage is unsupported");
    }
    let source =
        unsafe { core::slice::from_raw_parts(request.package_data, request.package_data_size) };
    let overlay_fill_rects =
        overlay_rects_from_raw(request.overlay_fill_rects, request.overlay_fill_rect_count)?;
    let overlay_line_rects =
        overlay_rects_from_raw(request.overlay_line_rects, request.overlay_line_rect_count)?;
    let overlay_motion_lines = overlay_rects_from_raw(
        request.overlay_motion_lines,
        request.overlay_motion_line_count,
    )?;
    render_package_to_metal_destination(
        renderer,
        request.destination_mtl_texture,
        request.flutter_mtl_texture,
        request.flutter_width,
        request.flutter_height,
        source,
        package,
        output,
        viewport_rect,
        request.sdr_white_scale,
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        request.overlay_generation,
    )
}

pub fn render_metal_cv_pixel_buffer_frame_set(
    request: &WgpuMetalCVPixelBufferRenderRequest,
) -> Result<(), &'static str> {
    let mut renderer = WgpuMetalRenderer::new()?;
    render_metal_cv_pixel_buffer_frame_set_with_renderer(&mut renderer, request)
}

pub fn render_metal_cv_pixel_buffer_frame_set_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalCVPixelBufferRenderRequest,
) -> Result<(), &'static str> {
    let submission = submit_metal_cv_pixel_buffer_frame_set_with_renderer(renderer, request)?;
    wait_for_submission(&renderer.device, submission, "wgpu-metal queue wait failed")
}

pub fn bake_metal_cv_pixel_buffer_frame_set_source_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalCVPixelBufferRenderRequest,
) -> Result<(), &'static str> {
    let submission =
        submit_metal_cv_pixel_buffer_frame_set_source_with_renderer(renderer, request)?;
    wait_for_submission(
        &renderer.device,
        submission,
        "wgpu-metal CVPixelBuffer source bake wait failed",
    )
}

pub fn bake_metal_cv_pixel_buffer_frame_set_source_with_renderer_async(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalCVPixelBufferRenderRequest,
    completion: WgpuMetalAsyncCompletion,
) -> Result<(), &'static str> {
    renderer.begin_profile_frame();
    let profile_start = Instant::now();
    let submission =
        match submit_metal_cv_pixel_buffer_frame_set_source_with_renderer(renderer, request) {
            Ok(submission) => submission,
            Err(error) => {
                renderer.profiler.last_cpu_render_us = profile_elapsed_us(profile_start);
                return Err(error);
            }
        };
    renderer.profiler.last_cpu_render_us = profile_elapsed_us(profile_start);
    renderer.submit_completion(submission, completion)
}

fn submit_metal_cv_pixel_buffer_frame_set_source_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalCVPixelBufferRenderRequest,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if request.frame_set.is_null() {
        return Err("wgpu-metal CVPixelBuffer frame set metadata is null");
    }
    if request.width <= 0 || request.height <= 0 {
        return Err("wgpu-metal CVPixelBuffer source bake dimensions are invalid");
    }
    let output = output_target_from_request(
        request.output_format,
        request.output_color_mode,
        request.width,
        request.height,
    )?;
    let frame_set = unsafe { &*(request.frame_set.cast::<CVPixelBufferPresentFrameSet>()) };
    let viewport_rect = composite_viewport_rect(
        request.width,
        request.height,
        request.viewport_left,
        request.viewport_top,
        request.viewport_right,
        request.viewport_bottom,
    );
    bake_cv_pixel_buffer_frame_set_source_atlas(
        renderer,
        &request.source_y_mtl_textures,
        &request.source_uv_mtl_textures,
        frame_set,
        output,
        viewport_rect,
        request.sdr_white_scale,
    )
}

fn submit_metal_cv_pixel_buffer_frame_set_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalCVPixelBufferRenderRequest,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if request.destination_mtl_texture.is_null() {
        return Err("wgpu-metal CVPixelBuffer destination texture is null");
    }
    if request.frame_set.is_null() {
        return Err("wgpu-metal CVPixelBuffer frame set metadata is null");
    }
    if request.width <= 0 || request.height <= 0 {
        return Err("wgpu-metal CVPixelBuffer dimensions are invalid");
    }
    let output = output_target_from_request(
        request.output_format,
        request.output_color_mode,
        request.width,
        request.height,
    )?;
    let frame_set = unsafe { &*(request.frame_set.cast::<CVPixelBufferPresentFrameSet>()) };
    let viewport_rect = composite_viewport_rect(
        request.width,
        request.height,
        request.viewport_left,
        request.viewport_top,
        request.viewport_right,
        request.viewport_bottom,
    );
    let overlay_fill_rects =
        overlay_rects_from_raw(request.overlay_fill_rects, request.overlay_fill_rect_count)?;
    let overlay_line_rects =
        overlay_rects_from_raw(request.overlay_line_rects, request.overlay_line_rect_count)?;
    let overlay_motion_lines = overlay_rects_from_raw(
        request.overlay_motion_lines,
        request.overlay_motion_line_count,
    )?;
    render_cv_pixel_buffer_frame_set_to_metal_destination(
        renderer,
        request.destination_mtl_texture,
        request.flutter_mtl_texture,
        request.flutter_width,
        request.flutter_height,
        &request.source_y_mtl_textures,
        &request.source_uv_mtl_textures,
        frame_set,
        output,
        viewport_rect,
        request.sdr_white_scale,
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        request.overlay_generation,
    )
}

pub fn composite_metal_retained_source_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRetainedCompositeRequest,
) -> Result<(), &'static str> {
    let submission = submit_metal_retained_source_with_renderer(renderer, request)?;
    wait_for_submission(&renderer.device, submission, "wgpu-metal queue wait failed")
}

pub fn composite_metal_retained_source_with_renderer_async(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRetainedCompositeRequest,
    completion: WgpuMetalAsyncCompletion,
) -> Result<(), &'static str> {
    renderer.begin_profile_frame();
    let profile_start = Instant::now();
    let submission = match submit_metal_retained_source_with_renderer(renderer, request) {
        Ok(submission) => submission,
        Err(error) => {
            renderer.profiler.last_cpu_render_us = profile_elapsed_us(profile_start);
            return Err(error);
        }
    };
    renderer.profiler.last_cpu_render_us = profile_elapsed_us(profile_start);
    renderer.submit_completion(submission, completion)
}

fn submit_metal_retained_source_with_renderer(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRetainedCompositeRequest,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if request.destination_mtl_texture.is_null() {
        return Err("wgpu-metal retained destination texture is null");
    }
    if request.decision.is_null() {
        return Err("wgpu-metal retained decision metadata is null");
    }
    let output = output_target_from_request(
        request.output_format,
        request.output_color_mode,
        request.width,
        request.height,
    )?;
    let decision = unsafe { &*(request.decision.cast::<PresentDecisionInfo>()) };
    let viewport_rect = composite_viewport_rect(
        request.width,
        request.height,
        request.viewport_left,
        request.viewport_top,
        request.viewport_right,
        request.viewport_bottom,
    );
    let overlay_fill_rects =
        overlay_rects_from_raw(request.overlay_fill_rects, request.overlay_fill_rect_count)?;
    let overlay_line_rects =
        overlay_rects_from_raw(request.overlay_line_rects, request.overlay_line_rect_count)?;
    let overlay_motion_lines = overlay_rects_from_raw(
        request.overlay_motion_lines,
        request.overlay_motion_line_count,
    )?;
    render_retained_source_to_metal_destination(
        renderer,
        request.destination_mtl_texture,
        request.flutter_mtl_texture,
        request.flutter_width,
        request.flutter_height,
        decision,
        output,
        viewport_rect,
        request.sdr_white_scale,
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        request.overlay_generation,
    )
}

fn wait_for_submission(
    device: &wgpu::Device,
    submission: wgpu::SubmissionIndex,
    error: &'static str,
) -> Result<(), &'static str> {
    device
        .poll(wgpu::PollType::Wait {
            submission_index: Some(submission),
            timeout: None,
        })
        .map_err(|_| error)?;
    Ok(())
}

pub static WGSL_COMPOSITE_SHADER: &str = include_str!("../shaders/composite.wgsl");

pub struct WgpuMetalRenderer {
    _instance: wgpu::Instance,
    adapter_info: wgpu::AdapterInfo,
    adapter_features: wgpu::Features,
    device: wgpu::Device,
    queue: wgpu::Queue,
    sampler: wgpu::Sampler,
    bind_group_layout: wgpu::BindGroupLayout,
    bgra8_pipeline: wgpu::RenderPipeline,
    rgba16_float_pipeline: wgpu::RenderPipeline,
    overlay_primitive_pipeline: wgpu::RenderPipeline,
    flutter_bgra8_pipeline: wgpu::RenderPipeline,
    flutter_rgba16_float_pipeline: wgpu::RenderPipeline,
    _dummy_bgra_array_texture: wgpu::Texture,
    dummy_bgra_array_view: wgpu::TextureView,
    _dummy_y_texture: wgpu::Texture,
    dummy_y_view: wgpu::TextureView,
    _dummy_uv_texture: wgpu::Texture,
    dummy_uv_view: wgpu::TextureView,
    _dummy_flutter_texture: wgpu::Texture,
    dummy_flutter_view: wgpu::TextureView,
    overlay_layer_texture: Option<CachedOverlayLayerTexture>,
    source_texture: Option<CachedSourceTexture>,
    params_buffer: Option<CachedStorageBuffer>,
    package_buffer: Option<CachedStorageBuffer>,
    package_buffer_dirty: bool,
    overlay_buffer: Option<CachedStorageBuffer>,
    overlay_layer_params_buffers: [Option<CachedStorageBuffer>; MAX_TRACKS],
    overlay_layer_views: [Option<CachedOverlayLayerView>; MAX_TRACKS],
    overlay_layer_bind_groups: [Option<CachedOverlayLayerBindGroup>; MAX_TRACKS],
    retained_storage: i32,
    retained_cv_y_textures: [Option<wgpu::TextureView>; MAX_TRACKS],
    retained_cv_uv_textures: [Option<wgpu::TextureView>; MAX_TRACKS],
    imported_textures: Vec<CachedImportedTexture>,
    import_clock: u64,
    resource_generation: u64,
    retained_source_generation: u64,
    generic_bind_group: Option<CachedBindGroup>,
    profiler: WgpuMetalProfilerSnapshot,
    source_bgra_scratch: Vec<u8>,
    params_scratch: Vec<u8>,
    package_bytes_scratch: Vec<u8>,
    overlay_rects_scratch: Vec<OverlayRect>,
    completion_tx: Option<mpsc::Sender<CompletionJob>>,
    completion_worker: Option<JoinHandle<()>>,
}

impl WgpuMetalRenderer {
    pub fn adapter_info(&self) -> &wgpu::AdapterInfo {
        &self.adapter_info
    }

    pub fn supports_texture_format_16bit_norm(&self) -> bool {
        self.adapter_features
            .contains(wgpu::Features::TEXTURE_FORMAT_16BIT_NORM)
    }

    pub fn profiler_snapshot(&self) -> WgpuMetalProfilerSnapshot {
        let mut snapshot = self.profiler;
        snapshot.imported_texture_cache_size = self.imported_textures.len() as u64;
        snapshot
    }

    fn begin_profile_frame(&mut self) {
        self.profiler.last_import_us = 0;
        self.profiler.last_prepare_us = 0;
        self.profiler.last_overlay_encode_us = 0;
        self.profiler.last_bind_group_us = 0;
        self.profiler.last_pass_encode_us = 0;
        self.profiler.last_submit_us = 0;
        self.profiler.last_cpu_render_us = 0;
    }

    fn import_metal_texture_2d_cached(
        &mut self,
        metal_texture: *mut core::ffi::c_void,
        format: wgpu::TextureFormat,
        width: u32,
        height: u32,
        usage: wgpu::TextureUsages,
        label: &'static str,
        texture_class: ImportedTextureClass,
    ) -> Result<(wgpu::Texture, wgpu::TextureView), &'static str> {
        if metal_texture.is_null() || width == 0 || height == 0 {
            return Err("wgpu-metal texture import arguments are invalid");
        }
        if texture_format_requires_16bit_norm(format) && !self.supports_texture_format_16bit_norm()
        {
            return Err(
                "wgpu-metal P010 texture import requires 16-bit normalized texture support",
            );
        }
        let profile_start = Instant::now();
        let key = ImportedTextureKey {
            metal_texture: metal_texture as usize,
            format: texture_format_key(format)?,
            width,
            height,
            usage_bits: usage.bits(),
        };
        self.import_clock = self.import_clock.wrapping_add(1).max(1);
        if let Some(entry) = self
            .imported_textures
            .iter_mut()
            .find(|entry| entry.key == key && entry.texture_class == texture_class)
        {
            entry.last_used = self.import_clock;
            match texture_class {
                ImportedTextureClass::Destination => {
                    self.profiler.destination_import_reuse_count += 1;
                }
                ImportedTextureClass::Source | ImportedTextureClass::Flutter => {
                    self.profiler.source_import_reuse_count += 1;
                }
            }
            self.profiler.last_import_us += profile_elapsed_us(profile_start);
            return Ok((entry.texture.clone(), entry.view.clone()));
        }

        let texture = match import_metal_texture_2d_raw(
            &self.device,
            metal_texture,
            format,
            width,
            height,
            usage,
            label,
        ) {
            Ok(texture) => texture,
            Err(error) => {
                self.profiler.last_import_us += profile_elapsed_us(profile_start);
                return Err(error);
            }
        };
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        match texture_class {
            ImportedTextureClass::Destination => {
                self.profiler.destination_import_count += 1;
            }
            ImportedTextureClass::Source | ImportedTextureClass::Flutter => {
                self.profiler.source_import_count += 1;
            }
        }
        self.imported_textures.push(CachedImportedTexture {
            key,
            texture_class,
            texture: texture.clone(),
            view: view.clone(),
            last_used: self.import_clock,
        });
        const MAX_DESTINATION_IMPORTED_TEXTURES: usize = 6;
        const MAX_SOURCE_IMPORTED_TEXTURES: usize = 16;
        const MAX_FLUTTER_IMPORTED_TEXTURES: usize = 6;
        let class_limit = match texture_class {
            ImportedTextureClass::Destination => MAX_DESTINATION_IMPORTED_TEXTURES,
            ImportedTextureClass::Source => MAX_SOURCE_IMPORTED_TEXTURES,
            ImportedTextureClass::Flutter => MAX_FLUTTER_IMPORTED_TEXTURES,
        };
        let class_count = self
            .imported_textures
            .iter()
            .filter(|entry| entry.texture_class == texture_class)
            .count();
        if class_count > class_limit {
            if let Some((evict_index, _)) = self
                .imported_textures
                .iter()
                .enumerate()
                .filter(|(_, entry)| entry.texture_class == texture_class)
                .min_by_key(|(_, entry)| entry.last_used)
            {
                self.imported_textures.swap_remove(evict_index);
                self.profiler.imported_texture_cache_eviction_count += 1;
            }
        }
        self.profiler.last_import_us += profile_elapsed_us(profile_start);
        Ok((texture, view))
    }

    fn submit_completion(
        &mut self,
        submission: wgpu::SubmissionIndex,
        completion: WgpuMetalAsyncCompletion,
    ) -> Result<(), &'static str> {
        let callback = completion
            .callback
            .ok_or("wgpu-metal async completion callback is null")?;
        if !completion.profiler_snapshot.is_null() {
            unsafe {
                *completion.profiler_snapshot = self.profiler_snapshot();
            }
        }
        let job = CompletionJob {
            submission,
            callback,
            user_data: completion.user_data as usize,
        };
        self.completion_tx
            .as_ref()
            .ok_or("wgpu-metal async completion worker is unavailable")?
            .send(job)
            .map_err(|_| "wgpu-metal async completion worker stopped")
    }
}

impl Drop for WgpuMetalRenderer {
    fn drop(&mut self) {
        self.completion_tx.take();
        if let Some(worker) = self.completion_worker.take() {
            let _ = worker.join();
        }
    }
}

struct CachedSourceTexture {
    width: u32,
    height: u32,
    layers: u32,
    format: wgpu::TextureFormat,
    generation: u64,
    texture: wgpu::Texture,
    view: wgpu::TextureView,
}

struct CachedStorageBuffer {
    capacity: u64,
    generation: u64,
    buffer: wgpu::Buffer,
}

struct CachedOverlayLayerTexture {
    width: u32,
    height: u32,
    layers: u32,
    overlay_generation: u64,
    fill_count: usize,
    line_count: usize,
    motion_count: usize,
    resource_generation: u64,
    texture: wgpu::Texture,
    view: wgpu::TextureView,
}

struct CachedOverlayLayerView {
    texture_generation: u64,
    view: wgpu::TextureView,
}

struct CachedOverlayLayerBindGroup {
    params_generation: u64,
    package_generation: u64,
    overlay_generation: u64,
    bind_group: wgpu::BindGroup,
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct BindGroupKey {
    source_storage: i32,
    params_generation: u64,
    package_generation: u64,
    overlay_generation: u64,
    source_generation: u64,
    overlay_layer_generation: u64,
    flutter_generation: u64,
}

struct CachedBindGroup {
    key: BindGroupKey,
    bind_group: wgpu::BindGroup,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ImportedTextureClass {
    Destination,
    Source,
    Flutter,
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct ImportedTextureKey {
    metal_texture: usize,
    format: u8,
    width: u32,
    height: u32,
    usage_bits: u32,
}

struct CachedImportedTexture {
    key: ImportedTextureKey,
    texture_class: ImportedTextureClass,
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    last_used: u64,
}

fn cv_plane_layout_entry(binding: u32) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Texture {
            sample_type: wgpu::TextureSampleType::Float { filterable: false },
            view_dimension: wgpu::TextureViewDimension::D2,
            multisampled: false,
        },
        count: None,
    }
}

fn create_composite_pipeline(
    device: &wgpu::Device,
    layout: &wgpu::PipelineLayout,
    shader: &wgpu::ShaderModule,
    format: wgpu::TextureFormat,
    label: &'static str,
) -> wgpu::RenderPipeline {
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some(label),
        layout: Some(layout),
        vertex: wgpu::VertexState {
            module: shader,
            entry_point: Some("vs_main"),
            buffers: &[],
            compilation_options: Default::default(),
        },
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: shader,
            entry_point: Some("fs_main"),
            targets: &[Some(wgpu::ColorTargetState {
                format,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
            compilation_options: Default::default(),
        }),
        multiview_mask: None,
        cache: None,
    })
}

fn create_overlay_primitive_pipeline(
    device: &wgpu::Device,
    layout: &wgpu::PipelineLayout,
    shader: &wgpu::ShaderModule,
    vertex_entry: &'static str,
    label: &'static str,
    blend: Option<wgpu::BlendState>,
) -> wgpu::RenderPipeline {
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some(label),
        layout: Some(layout),
        vertex: wgpu::VertexState {
            module: shader,
            entry_point: Some(vertex_entry),
            buffers: &[],
            compilation_options: Default::default(),
        },
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: shader,
            entry_point: Some("fs_overlay_primitive"),
            targets: &[Some(wgpu::ColorTargetState {
                format: wgpu::TextureFormat::Rgba8Unorm,
                blend,
                write_mask: wgpu::ColorWrites::ALL,
            })],
            compilation_options: Default::default(),
        }),
        multiview_mask: None,
        cache: None,
    })
}

fn create_flutter_surface_pipeline(
    device: &wgpu::Device,
    layout: &wgpu::PipelineLayout,
    shader: &wgpu::ShaderModule,
    format: wgpu::TextureFormat,
    blend: Option<wgpu::BlendState>,
    label: &'static str,
) -> wgpu::RenderPipeline {
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some(label),
        layout: Some(layout),
        vertex: wgpu::VertexState {
            module: shader,
            entry_point: Some("vs_main"),
            buffers: &[],
            compilation_options: Default::default(),
        },
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: shader,
            entry_point: Some("fs_flutter_surface"),
            targets: &[Some(wgpu::ColorTargetState {
                format,
                blend,
                write_mask: wgpu::ColorWrites::ALL,
            })],
            compilation_options: Default::default(),
        }),
        multiview_mask: None,
        cache: None,
    })
}

fn composite_pipeline_for_output<'a>(
    renderer: &'a WgpuMetalRenderer,
    output_format: wgpu::TextureFormat,
) -> Result<&'a wgpu::RenderPipeline, &'static str> {
    match output_format {
        wgpu::TextureFormat::Bgra8Unorm => Ok(&renderer.bgra8_pipeline),
        wgpu::TextureFormat::Rgba16Float => Ok(&renderer.rgba16_float_pipeline),
        _ => Err("wgpu-metal output pipeline format is unsupported"),
    }
}

fn flutter_pipeline_for_output<'a>(
    renderer: &'a WgpuMetalRenderer,
    output_format: wgpu::TextureFormat,
) -> Result<&'a wgpu::RenderPipeline, &'static str> {
    match output_format {
        wgpu::TextureFormat::Bgra8Unorm => Ok(&renderer.flutter_bgra8_pipeline),
        wgpu::TextureFormat::Rgba16Float => Ok(&renderer.flutter_rgba16_float_pipeline),
        _ => Err("wgpu-metal Flutter pipeline format is unsupported"),
    }
}

impl WgpuMetalRenderer {
    pub fn new() -> Result<Self, &'static str> {
        pollster::block_on(Self::new_async())
    }

    pub fn metal_device_ptr(&self) -> *mut core::ffi::c_void {
        unsafe {
            self.device
                .as_hal::<wgpu_hal::metal::Api>()
                .map(|device| Retained::as_ptr(device.raw_device()).cast_mut().cast())
                .unwrap_or(core::ptr::null_mut())
        }
    }

    async fn new_async() -> Result<Self, &'static str> {
        let mut instance_desc = wgpu::InstanceDescriptor::new_without_display_handle();
        instance_desc.backends = wgpu::Backends::METAL;
        let instance = wgpu::Instance::new(instance_desc);
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions::default())
            .await
            .map_err(|_| "wgpu-metal failed to create Metal adapter")?;
        let adapter_features = adapter.features();
        let enabled_optional_features =
            adapter_features & wgpu::Features::TEXTURE_FORMAT_16BIT_NORM;
        let adapter_limits = adapter.limits();
        let mut required_limits = wgpu::Limits::downlevel_defaults();
        required_limits.max_texture_dimension_2d = adapter_limits.max_texture_dimension_2d;
        let adapter_info = adapter.get_info();
        let (device, queue) = adapter
            .request_device(&wgpu::DeviceDescriptor {
                label: Some("voidplayer-wgpu-metal"),
                required_features: enabled_optional_features,
                required_limits,
                experimental_features: wgpu::ExperimentalFeatures::disabled(),
                memory_hints: wgpu::MemoryHints::default(),
                trace: wgpu::Trace::Off,
            })
            .await
            .map_err(|_| "wgpu-metal failed to create Metal device")?;
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("voidplayer-wgpu-composite-sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("voidplayer-wgpu-composite-wgsl"),
            source: wgpu::ShaderSource::Wgsl(WGSL_COMPOSITE_SHADER.into()),
        });
        let bind_group_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("voidplayer-wgpu-composite-bind-group-layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                cv_plane_layout_entry(5),
                cv_plane_layout_entry(6),
                cv_plane_layout_entry(7),
                cv_plane_layout_entry(8),
                cv_plane_layout_entry(9),
                cv_plane_layout_entry(10),
                cv_plane_layout_entry(11),
                cv_plane_layout_entry(12),
                wgpu::BindGroupLayoutEntry {
                    binding: 13,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 14,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("voidplayer-wgpu-composite-pipeline-layout"),
            bind_group_layouts: &[Some(&bind_group_layout)],
            immediate_size: 0,
        });
        let bgra8_pipeline = create_composite_pipeline(
            &device,
            &pipeline_layout,
            &shader,
            wgpu::TextureFormat::Bgra8Unorm,
            "voidplayer-wgpu-composite-bgra8-pipeline",
        );
        let rgba16_float_pipeline = create_composite_pipeline(
            &device,
            &pipeline_layout,
            &shader,
            wgpu::TextureFormat::Rgba16Float,
            "voidplayer-wgpu-composite-rgba16float-pipeline",
        );
        let overlay_blend = Some(wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::SrcAlpha,
                dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                operation: wgpu::BlendOperation::Add,
            },
            alpha: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::One,
                dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                operation: wgpu::BlendOperation::Add,
            },
        });
        let overlay_primitive_pipeline = create_overlay_primitive_pipeline(
            &device,
            &pipeline_layout,
            &shader,
            "vs_overlay_primitive",
            "voidplayer-wgpu-overlay-primitive-layer-pipeline",
            overlay_blend,
        );
        let flutter_blend = Some(wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::One,
                dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                operation: wgpu::BlendOperation::Add,
            },
            alpha: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::One,
                dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                operation: wgpu::BlendOperation::Add,
            },
        });
        let flutter_bgra8_pipeline = create_flutter_surface_pipeline(
            &device,
            &pipeline_layout,
            &shader,
            wgpu::TextureFormat::Bgra8Unorm,
            flutter_blend,
            "voidplayer-wgpu-bgra8-flutter-surface-pipeline",
        );
        let flutter_rgba16_float_pipeline = create_flutter_surface_pipeline(
            &device,
            &pipeline_layout,
            &shader,
            wgpu::TextureFormat::Rgba16Float,
            flutter_blend,
            "voidplayer-wgpu-rgba16float-flutter-surface-pipeline",
        );
        let dummy_bgra_array_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("voidplayer-wgpu-dummy-bgra-array"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: MAX_TRACKS as u32,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Bgra8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_bgra_array_view =
            dummy_bgra_array_texture.create_view(&wgpu::TextureViewDescriptor::default());
        let dummy_y_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("voidplayer-wgpu-dummy-cv-y"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_y_view = dummy_y_texture.create_view(&wgpu::TextureViewDescriptor::default());
        let dummy_uv_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("voidplayer-wgpu-dummy-cv-uv"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rg8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let dummy_uv_view = dummy_uv_texture.create_view(&wgpu::TextureViewDescriptor::default());
        let dummy_flutter_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("voidplayer-wgpu-dummy-flutter-surface"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Bgra8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let dummy_flutter_view =
            dummy_flutter_texture.create_view(&wgpu::TextureViewDescriptor::default());
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &dummy_flutter_texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &[0, 0, 0, 0],
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(4),
                rows_per_image: Some(1),
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        let (completion_tx, completion_rx) = mpsc::channel::<CompletionJob>();
        let completion_device = device.clone();
        let completion_worker = thread::Builder::new()
            .name("voidplayer-wgpu-completion".to_string())
            .spawn(move || {
                while let Ok(job) = completion_rx.recv() {
                    let success = completion_device
                        .poll(wgpu::PollType::Wait {
                            submission_index: Some(job.submission),
                            timeout: None,
                        })
                        .is_ok();
                    (job.callback)(
                        job.user_data as *mut core::ffi::c_void,
                        if success { 0 } else { -1 },
                    );
                }
            })
            .map_err(|_| "wgpu-metal async completion worker spawn failed")?;
        Ok(Self {
            _instance: instance,
            adapter_info,
            adapter_features: enabled_optional_features,
            device,
            queue,
            sampler,
            bind_group_layout,
            bgra8_pipeline,
            rgba16_float_pipeline,
            overlay_primitive_pipeline,
            flutter_bgra8_pipeline,
            flutter_rgba16_float_pipeline,
            _dummy_bgra_array_texture: dummy_bgra_array_texture,
            dummy_bgra_array_view,
            _dummy_y_texture: dummy_y_texture,
            dummy_y_view,
            _dummy_uv_texture: dummy_uv_texture,
            dummy_uv_view,
            _dummy_flutter_texture: dummy_flutter_texture,
            dummy_flutter_view,
            overlay_layer_texture: None,
            source_texture: None,
            params_buffer: None,
            package_buffer: None,
            package_buffer_dirty: false,
            overlay_buffer: None,
            overlay_layer_params_buffers: std::array::from_fn(|_| None),
            overlay_layer_views: std::array::from_fn(|_| None),
            overlay_layer_bind_groups: std::array::from_fn(|_| None),
            retained_storage: STORAGE_NONE,
            retained_cv_y_textures: std::array::from_fn(|_| None),
            retained_cv_uv_textures: std::array::from_fn(|_| None),
            imported_textures: Vec::new(),
            import_clock: 0,
            resource_generation: 0,
            retained_source_generation: 0,
            generic_bind_group: None,
            profiler: WgpuMetalProfilerSnapshot::default(),
            source_bgra_scratch: Vec::new(),
            params_scratch: Vec::new(),
            package_bytes_scratch: Vec::new(),
            overlay_rects_scratch: Vec::new(),
            completion_tx: Some(completion_tx),
            completion_worker: Some(completion_worker),
        })
    }
}

fn overlay_rects_from_raw<'a>(
    ptr: *const OverlayRect,
    count: usize,
) -> Result<&'a [OverlayRect], &'static str> {
    if count == 0 {
        return Ok(&[]);
    }
    if ptr.is_null() {
        return Err("wgpu-metal overlay rect pointer is null");
    }
    Ok(unsafe { core::slice::from_raw_parts(ptr, count) })
}

fn texture_format_key(format: wgpu::TextureFormat) -> Result<u8, &'static str> {
    match format {
        wgpu::TextureFormat::Bgra8Unorm => Ok(1),
        wgpu::TextureFormat::Rgba16Float => Ok(2),
        wgpu::TextureFormat::R8Unorm => Ok(3),
        wgpu::TextureFormat::Rg8Unorm => Ok(4),
        wgpu::TextureFormat::R16Unorm => Ok(5),
        wgpu::TextureFormat::Rg16Unorm => Ok(6),
        _ => Err("wgpu-metal texture import format is unsupported"),
    }
}

fn texture_format_requires_16bit_norm(format: wgpu::TextureFormat) -> bool {
    matches!(
        format,
        wgpu::TextureFormat::R16Unorm | wgpu::TextureFormat::Rg16Unorm
    )
}

fn metal_pixel_format_for_wgpu(
    format: wgpu::TextureFormat,
) -> Result<MTLPixelFormat, &'static str> {
    match format {
        wgpu::TextureFormat::Bgra8Unorm => Ok(MTLPixelFormat::BGRA8Unorm),
        wgpu::TextureFormat::Rgba16Float => Ok(MTLPixelFormat::RGBA16Float),
        wgpu::TextureFormat::R8Unorm => Ok(MTLPixelFormat::R8Unorm),
        wgpu::TextureFormat::Rg8Unorm => Ok(MTLPixelFormat::RG8Unorm),
        wgpu::TextureFormat::R16Unorm => Ok(MTLPixelFormat::R16Unorm),
        wgpu::TextureFormat::Rg16Unorm => Ok(MTLPixelFormat::RG16Unorm),
        _ => Err("wgpu-metal texture import format is unsupported"),
    }
}

fn import_metal_texture_2d_raw(
    device: &wgpu::Device,
    metal_texture: *mut core::ffi::c_void,
    format: wgpu::TextureFormat,
    width: u32,
    height: u32,
    usage: wgpu::TextureUsages,
    label: &'static str,
) -> Result<wgpu::Texture, &'static str> {
    if metal_texture.is_null() || width == 0 || height == 0 {
        return Err("wgpu-metal texture import arguments are invalid");
    }
    let raw_texture =
        unsafe { Retained::retain(metal_texture.cast::<ProtocolObject<dyn MTLTexture>>()) }
            .ok_or("wgpu-metal failed to retain MTLTexture")?;
    let expected_device = unsafe {
        device
            .as_hal::<wgpu_hal::metal::Api>()
            .map(|device| Retained::as_ptr(device.raw_device()).cast::<core::ffi::c_void>())
    }
    .ok_or("wgpu-metal failed to query wgpu Metal device")?;
    let texture_device = raw_texture.device();
    let texture_device_ptr: *const core::ffi::c_void = Retained::as_ptr(&texture_device).cast();
    if texture_device_ptr != expected_device {
        return Err("wgpu-metal MTLTexture device does not match wgpu Metal device");
    }
    if raw_texture.textureType() != MTLTextureType::Type2D
        || raw_texture.pixelFormat() != metal_pixel_format_for_wgpu(format)?
        || raw_texture.width() != width as usize
        || raw_texture.height() != height as usize
        || raw_texture.mipmapLevelCount() != 1
        || raw_texture.sampleCount() != 1
    {
        return Err("wgpu-metal MTLTexture descriptor does not match import request");
    }
    let size = wgpu::Extent3d {
        width,
        height,
        depth_or_array_layers: 1,
    };
    let hal_texture = unsafe {
        wgpu_hal::metal::Device::texture_from_raw(
            raw_texture,
            format,
            MTLTextureType::Type2D,
            1,
            1,
            wgpu_hal::CopyExtent::from(size),
        )
    };
    let desc = wgpu::TextureDescriptor {
        label: Some(label),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage,
        view_formats: &[],
    };
    Ok(unsafe { device.create_texture_from_hal::<wgpu_hal::metal::Api>(hal_texture, &desc) })
}

struct ImportedFlutterSurface {
    view: wgpu::TextureView,
    generation: u64,
    width: u32,
    height: u32,
}

fn flutter_surface_generation(
    metal_texture: *mut core::ffi::c_void,
    width: u32,
    height: u32,
) -> u64 {
    0xcbf29ce484222325u64
        .wrapping_mul(1099511628211)
        .wrapping_add(metal_texture as usize as u64)
        .wrapping_mul(1099511628211)
        .wrapping_add((width as u64) << 32)
        .wrapping_add(height as u64)
        .max(1)
}

fn import_flutter_surface(
    renderer: &mut WgpuMetalRenderer,
    flutter_mtl_texture: *mut core::ffi::c_void,
    flutter_width: i32,
    flutter_height: i32,
) -> Result<Option<ImportedFlutterSurface>, &'static str> {
    if flutter_mtl_texture.is_null() {
        return Ok(None);
    }
    if flutter_width <= 0 || flutter_height <= 0 {
        return Err("wgpu-metal Flutter surface dimensions are invalid");
    }
    let width = flutter_width as u32;
    let height = flutter_height as u32;
    let (_texture, view) = renderer.import_metal_texture_2d_cached(
        flutter_mtl_texture,
        wgpu::TextureFormat::Bgra8Unorm,
        width,
        height,
        wgpu::TextureUsages::TEXTURE_BINDING,
        "voidplayer-imported-flutter-surface",
        ImportedTextureClass::Flutter,
    )?;
    Ok(Some(ImportedFlutterSurface {
        view,
        generation: flutter_surface_generation(flutter_mtl_texture, width, height),
        width,
        height,
    }))
}

fn normalized_viewport_component(value: f32, fallback: f32) -> f32 {
    if value.is_finite() {
        value
    } else {
        fallback
    }
}

fn composite_viewport_rect(
    output_width: i32,
    output_height: i32,
    viewport_left: f32,
    viewport_top: f32,
    viewport_right: f32,
    viewport_bottom: f32,
) -> [f32; 4] {
    let output_width = output_width.max(1) as f32;
    let output_height = output_height.max(1) as f32;
    let left = normalized_viewport_component(viewport_left, 0.0).clamp(0.0, 1.0);
    let top = normalized_viewport_component(viewport_top, 0.0).clamp(0.0, 1.0);
    let raw_right = normalized_viewport_component(viewport_right, 1.0);
    let raw_bottom = normalized_viewport_component(viewport_bottom, 1.0);
    if raw_right <= left || raw_bottom <= top {
        return [0.0, 0.0, output_width, output_height];
    }
    let right = raw_right.clamp(left, 1.0);
    let bottom = raw_bottom.clamp(top, 1.0);
    let width = ((right - left) * output_width).max(1.0);
    let height = ((bottom - top) * output_height).max(1.0);
    [left * output_width, top * output_height, width, height]
}

fn bake_bgra_package_source_atlas(
    renderer: &mut WgpuMetalRenderer,
    source: &[u8],
    package: &PresentFramePackageInfo,
    output: OutputTarget,
    sdr_white_scale: f32,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    let prepare_start = Instant::now();
    let (bgra_atlas_width, bgra_atlas_height) =
        bgra_atlas_for_wgsl(source, package, &mut renderer.source_bgra_scratch)?;
    renderer.params_scratch.clear();
    package_params(
        package,
        STORAGE_BGRA,
        output.color_mode,
        sdr_white_scale,
        output.width as i32,
        output.height as i32,
        [
            0.0,
            0.0,
            output.width.max(1) as f32,
            output.height.max(1) as f32,
        ],
        0,
        0,
        &[],
        &[],
        &[],
        0.0,
        &mut renderer.params_scratch,
    );
    renderer.package_buffer_dirty |=
        set_dummy_package_storage_bytes(&mut renderer.package_bytes_scratch);
    renderer.overlay_rects_scratch.clear();
    write_source_bgra_atlas(
        &renderer.device,
        &renderer.queue,
        &mut renderer.source_texture,
        &mut renderer.resource_generation,
        bgra_atlas_width,
        bgra_atlas_height,
        &renderer.source_bgra_scratch,
    )?;
    renderer.retained_storage = STORAGE_BGRA;
    renderer.retained_cv_y_textures = std::array::from_fn(|_| None);
    renderer.retained_cv_uv_textures = std::array::from_fn(|_| None);
    renderer.profiler.last_prepare_us += profile_elapsed_us(prepare_start);
    let submit_start = Instant::now();
    let submission = renderer
        .queue
        .submit(std::iter::empty::<wgpu::CommandBuffer>());
    renderer.profiler.last_submit_us += profile_elapsed_us(submit_start);
    renderer.profiler.submit_count += 1;
    Ok(submission)
}

fn render_package_to_metal_destination(
    renderer: &mut WgpuMetalRenderer,
    destination_mtl_texture: *mut core::ffi::c_void,
    flutter_mtl_texture: *mut core::ffi::c_void,
    flutter_width: i32,
    flutter_height: i32,
    source: &[u8],
    package: &PresentFramePackageInfo,
    output: OutputTarget,
    viewport_rect: [f32; 4],
    sdr_white_scale: f32,
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    let (_destination_texture, destination_view) = renderer.import_metal_texture_2d_cached(
        destination_mtl_texture,
        output.format,
        output.width,
        output.height,
        wgpu::TextureUsages::RENDER_ATTACHMENT,
        "voidplayer-imported-cvpixelbuffer-destination",
        ImportedTextureClass::Destination,
    )?;
    let flutter_surface =
        import_flutter_surface(renderer, flutter_mtl_texture, flutter_width, flutter_height)?;

    let prepare_start = Instant::now();
    renderer.params_scratch.clear();
    if package.storage == STORAGE_BGRA {
        let (bgra_atlas_width, bgra_atlas_height) =
            bgra_atlas_for_wgsl(source, package, &mut renderer.source_bgra_scratch)?;
        package_params(
            package,
            STORAGE_BGRA,
            output.color_mode,
            sdr_white_scale,
            output.width as i32,
            output.height as i32,
            viewport_rect,
            flutter_surface.as_ref().map_or(0, |surface| surface.width),
            flutter_surface.as_ref().map_or(0, |surface| surface.height),
            overlay_fill_rects,
            overlay_line_rects,
            overlay_motion_lines,
            0.0,
            &mut renderer.params_scratch,
        );
        renderer.package_buffer_dirty |=
            set_dummy_package_storage_bytes(&mut renderer.package_bytes_scratch);
        write_source_bgra_atlas(
            &renderer.device,
            &renderer.queue,
            &mut renderer.source_texture,
            &mut renderer.resource_generation,
            bgra_atlas_width,
            bgra_atlas_height,
            &renderer.source_bgra_scratch,
        )?;
    } else {
        package_params(
            package,
            STORAGE_YUV,
            output.color_mode,
            sdr_white_scale,
            output.width as i32,
            output.height as i32,
            viewport_rect,
            flutter_surface.as_ref().map_or(0, |surface| surface.width),
            flutter_surface.as_ref().map_or(0, |surface| surface.height),
            overlay_fill_rects,
            overlay_line_rects,
            overlay_motion_lines,
            0.0,
            &mut renderer.params_scratch,
        );
        package_storage_bytes(source, &mut renderer.package_bytes_scratch);
        renderer.package_buffer_dirty = true;
    }
    combined_overlay_rects(
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        &mut renderer.overlay_rects_scratch,
    );
    renderer.profiler.last_prepare_us += profile_elapsed_us(prepare_start);

    let submission = render_bgra_atlas_with_wgsl(
        renderer,
        &destination_view,
        package.storage,
        output.format,
        output.width,
        output.height,
        overlay_generation,
        flutter_surface.as_ref().map(|surface| &surface.view),
        flutter_surface
            .as_ref()
            .map_or(0, |surface| surface.generation),
    )?;
    renderer.retained_storage = package.storage;
    renderer.retained_cv_y_textures = std::array::from_fn(|_| None);
    renderer.retained_cv_uv_textures = std::array::from_fn(|_| None);
    Ok(submission)
}

fn render_cv_pixel_buffer_frame_set_to_metal_destination(
    renderer: &mut WgpuMetalRenderer,
    destination_mtl_texture: *mut core::ffi::c_void,
    flutter_mtl_texture: *mut core::ffi::c_void,
    flutter_width: i32,
    flutter_height: i32,
    source_y_mtl_textures: &[*mut core::ffi::c_void; MAX_TRACKS],
    source_uv_mtl_textures: &[*mut core::ffi::c_void; MAX_TRACKS],
    frame_set: &CVPixelBufferPresentFrameSet,
    output: OutputTarget,
    viewport_rect: [f32; 4],
    sdr_white_scale: f32,
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if frame_set.decision.should_present == 0 || frame_set.decision.frame_count <= 0 {
        return Err("wgpu-metal CVPixelBuffer frame set has no presentable frame");
    }
    let (_destination_texture, destination_view) = renderer.import_metal_texture_2d_cached(
        destination_mtl_texture,
        output.format,
        output.width,
        output.height,
        wgpu::TextureUsages::RENDER_ATTACHMENT,
        "voidplayer-imported-cvpixelbuffer-destination",
        ImportedTextureClass::Destination,
    )?;
    let flutter_surface =
        import_flutter_surface(renderer, flutter_mtl_texture, flutter_width, flutter_height)?;
    let mut source_y_textures: [Option<wgpu::TextureView>; MAX_TRACKS] =
        std::array::from_fn(|_| None);
    let mut source_uv_textures: [Option<wgpu::TextureView>; MAX_TRACKS] =
        std::array::from_fn(|_| None);
    let mut source_generation = 0x9e3779b97f4a7c15u64;
    for slot in 0..MAX_TRACKS {
        if frame_set.decision.frames[slot].present == 0 {
            continue;
        }
        if source_y_mtl_textures[slot].is_null() || source_uv_mtl_textures[slot].is_null() {
            return Err("wgpu-metal CVPixelBuffer source plane texture is null");
        }
        if frame_set.plane_counts[slot] < 2
            || frame_set.coded_widths[slot] <= 0
            || frame_set.coded_heights[slot] <= 0
        {
            return Err("wgpu-metal CVPixelBuffer frame set metadata is invalid");
        }
        let coded_width = frame_set.coded_widths[slot] as u32;
        let coded_height = frame_set.coded_heights[slot] as u32;
        let chroma_width = coded_width.div_ceil(2).max(1);
        let chroma_height = coded_height.div_ceil(2).max(1);
        let is_p010 = frame_set.is_p010[slot] != 0;
        source_generation = source_generation
            .wrapping_mul(1099511628211)
            .wrapping_add(((slot as u64) + 1) << 56)
            .wrapping_add(source_y_mtl_textures[slot] as usize as u64)
            .wrapping_mul(1099511628211)
            .wrapping_add(source_uv_mtl_textures[slot] as usize as u64)
            .wrapping_mul(1099511628211)
            .wrapping_add((coded_width as u64) << 32)
            .wrapping_add(coded_height as u64)
            .wrapping_mul(1099511628211)
            .wrapping_add(if is_p010 { 1 } else { 0 });
        source_y_textures[slot] = Some(
            renderer
                .import_metal_texture_2d_cached(
                    source_y_mtl_textures[slot],
                    if is_p010 {
                        wgpu::TextureFormat::R16Unorm
                    } else {
                        wgpu::TextureFormat::R8Unorm
                    },
                    coded_width,
                    coded_height,
                    wgpu::TextureUsages::TEXTURE_BINDING,
                    "voidplayer-imported-cvpixelbuffer-source-y",
                    ImportedTextureClass::Source,
                )?
                .1,
        );
        source_uv_textures[slot] = Some(
            renderer
                .import_metal_texture_2d_cached(
                    source_uv_mtl_textures[slot],
                    if is_p010 {
                        wgpu::TextureFormat::Rg16Unorm
                    } else {
                        wgpu::TextureFormat::Rg8Unorm
                    },
                    chroma_width,
                    chroma_height,
                    wgpu::TextureUsages::TEXTURE_BINDING,
                    "voidplayer-imported-cvpixelbuffer-source-uv",
                    ImportedTextureClass::Source,
                )?
                .1,
        );
    }

    let prepare_start = Instant::now();
    let viewport_width = viewport_rect[2].round().max(1.0) as i32;
    let viewport_height = viewport_rect[3].round().max(1.0) as i32;
    let package = PresentFramePackageInfo {
        storage: STORAGE_CV_PIXEL_BUFFER,
        width: viewport_width,
        height: viewport_height,
        max_track_slots: MAX_TRACKS as i32,
        stride_bytes: 0,
        track_stride_bytes: 0,
        used_bytes: 4,
        decision: frame_set.decision,
    };
    renderer.params_scratch.clear();
    package_params(
        &package,
        STORAGE_OUTPUT_ATLAS,
        output.color_mode,
        sdr_white_scale,
        output.width as i32,
        output.height as i32,
        viewport_rect,
        flutter_surface.as_ref().map_or(0, |surface| surface.width),
        flutter_surface.as_ref().map_or(0, |surface| surface.height),
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        0.0,
        &mut renderer.params_scratch,
    );
    renderer.package_buffer_dirty |=
        set_dummy_package_storage_bytes(&mut renderer.package_bytes_scratch);
    combined_overlay_rects(
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        &mut renderer.overlay_rects_scratch,
    );
    renderer.retained_source_generation = source_generation.max(1);
    renderer.profiler.last_prepare_us += profile_elapsed_us(prepare_start);
    let submission = render_cv_pixel_buffer_frame_set_with_wgsl(
        renderer,
        &destination_view,
        &source_y_textures,
        &source_uv_textures,
        frame_set,
        output.color_mode,
        output.format,
        output.width,
        output.height,
        overlay_generation,
        flutter_surface.as_ref().map(|surface| &surface.view),
        flutter_surface
            .as_ref()
            .map_or(0, |surface| surface.generation),
    )?;
    renderer.retained_storage = STORAGE_OUTPUT_ATLAS;
    renderer.retained_cv_y_textures = std::array::from_fn(|_| None);
    renderer.retained_cv_uv_textures = std::array::from_fn(|_| None);
    Ok(submission)
}

fn bake_cv_pixel_buffer_frame_set_source_atlas(
    renderer: &mut WgpuMetalRenderer,
    source_y_mtl_textures: &[*mut core::ffi::c_void; MAX_TRACKS],
    source_uv_mtl_textures: &[*mut core::ffi::c_void; MAX_TRACKS],
    frame_set: &CVPixelBufferPresentFrameSet,
    output: OutputTarget,
    viewport_rect: [f32; 4],
    sdr_white_scale: f32,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if frame_set.decision.should_present == 0 || frame_set.decision.frame_count <= 0 {
        return Err("wgpu-metal CVPixelBuffer frame set has no presentable frame");
    }
    let mut source_y_textures: [Option<wgpu::TextureView>; MAX_TRACKS] =
        std::array::from_fn(|_| None);
    let mut source_uv_textures: [Option<wgpu::TextureView>; MAX_TRACKS] =
        std::array::from_fn(|_| None);
    let mut source_generation = 0x9e3779b97f4a7c15u64;
    for slot in 0..MAX_TRACKS {
        if frame_set.decision.frames[slot].present == 0 {
            continue;
        }
        if source_y_mtl_textures[slot].is_null() || source_uv_mtl_textures[slot].is_null() {
            return Err("wgpu-metal CVPixelBuffer source plane texture is null");
        }
        if frame_set.plane_counts[slot] < 2
            || frame_set.coded_widths[slot] <= 0
            || frame_set.coded_heights[slot] <= 0
        {
            return Err("wgpu-metal CVPixelBuffer frame set metadata is invalid");
        }
        let coded_width = frame_set.coded_widths[slot] as u32;
        let coded_height = frame_set.coded_heights[slot] as u32;
        let chroma_width = coded_width.div_ceil(2).max(1);
        let chroma_height = coded_height.div_ceil(2).max(1);
        let is_p010 = frame_set.is_p010[slot] != 0;
        source_generation = source_generation
            .wrapping_mul(1099511628211)
            .wrapping_add(((slot as u64) + 1) << 56)
            .wrapping_add(source_y_mtl_textures[slot] as usize as u64)
            .wrapping_mul(1099511628211)
            .wrapping_add(source_uv_mtl_textures[slot] as usize as u64)
            .wrapping_mul(1099511628211)
            .wrapping_add((coded_width as u64) << 32)
            .wrapping_add(coded_height as u64)
            .wrapping_mul(1099511628211)
            .wrapping_add(if is_p010 { 1 } else { 0 });
        source_y_textures[slot] = Some(
            renderer
                .import_metal_texture_2d_cached(
                    source_y_mtl_textures[slot],
                    if is_p010 {
                        wgpu::TextureFormat::R16Unorm
                    } else {
                        wgpu::TextureFormat::R8Unorm
                    },
                    coded_width,
                    coded_height,
                    wgpu::TextureUsages::TEXTURE_BINDING,
                    "voidplayer-imported-cvpixelbuffer-source-y",
                    ImportedTextureClass::Source,
                )?
                .1,
        );
        source_uv_textures[slot] = Some(
            renderer
                .import_metal_texture_2d_cached(
                    source_uv_mtl_textures[slot],
                    if is_p010 {
                        wgpu::TextureFormat::Rg16Unorm
                    } else {
                        wgpu::TextureFormat::Rg8Unorm
                    },
                    chroma_width,
                    chroma_height,
                    wgpu::TextureUsages::TEXTURE_BINDING,
                    "voidplayer-imported-cvpixelbuffer-source-uv",
                    ImportedTextureClass::Source,
                )?
                .1,
        );
    }

    let prepare_start = Instant::now();
    let viewport_width = viewport_rect[2].round().max(1.0) as i32;
    let viewport_height = viewport_rect[3].round().max(1.0) as i32;
    let package = PresentFramePackageInfo {
        storage: STORAGE_CV_PIXEL_BUFFER,
        width: viewport_width,
        height: viewport_height,
        max_track_slots: MAX_TRACKS as i32,
        stride_bytes: 0,
        track_stride_bytes: 0,
        used_bytes: 4,
        decision: frame_set.decision,
    };
    renderer.params_scratch.clear();
    package_params(
        &package,
        STORAGE_OUTPUT_ATLAS,
        output.color_mode,
        sdr_white_scale,
        output.width as i32,
        output.height as i32,
        viewport_rect,
        0,
        0,
        &[],
        &[],
        &[],
        0.0,
        &mut renderer.params_scratch,
    );
    renderer.package_buffer_dirty |=
        set_dummy_package_storage_bytes(&mut renderer.package_bytes_scratch);
    renderer.overlay_rects_scratch.clear();
    renderer.retained_source_generation = source_generation.max(1);
    renderer.profiler.last_prepare_us += profile_elapsed_us(prepare_start);
    let mut encoder = renderer
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("voidplayer-wgpu-cvpixelbuffer-source-only-bake-encoder"),
        });
    encode_cv_source_output_atlas(
        renderer,
        &mut encoder,
        &source_y_textures,
        &source_uv_textures,
        frame_set,
        output,
        sdr_white_scale,
    )?;
    renderer.retained_storage = STORAGE_OUTPUT_ATLAS;
    renderer.retained_cv_y_textures = std::array::from_fn(|_| None);
    renderer.retained_cv_uv_textures = std::array::from_fn(|_| None);
    let submit_start = Instant::now();
    let submission = renderer.queue.submit(std::iter::once(encoder.finish()));
    renderer.profiler.last_submit_us += profile_elapsed_us(submit_start);
    renderer.profiler.submit_count += 1;
    Ok(submission)
}

fn render_retained_source_to_metal_destination(
    renderer: &mut WgpuMetalRenderer,
    destination_mtl_texture: *mut core::ffi::c_void,
    flutter_mtl_texture: *mut core::ffi::c_void,
    flutter_width: i32,
    flutter_height: i32,
    decision: &PresentDecisionInfo,
    output: OutputTarget,
    viewport_rect: [f32; 4],
    sdr_white_scale: f32,
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if renderer.retained_storage == STORAGE_NONE {
        return Err("wgpu-metal retained source cache is empty");
    }
    let (_destination_texture, destination_view) = renderer.import_metal_texture_2d_cached(
        destination_mtl_texture,
        output.format,
        output.width,
        output.height,
        wgpu::TextureUsages::RENDER_ATTACHMENT,
        "voidplayer-imported-retained-composite-destination",
        ImportedTextureClass::Destination,
    )?;
    let flutter_surface =
        import_flutter_surface(renderer, flutter_mtl_texture, flutter_width, flutter_height)?;
    let prepare_start = Instant::now();
    let viewport_width = viewport_rect[2].round().max(1.0) as i32;
    let viewport_height = viewport_rect[3].round().max(1.0) as i32;
    let package = PresentFramePackageInfo {
        storage: renderer.retained_storage,
        width: viewport_width,
        height: viewport_height,
        max_track_slots: MAX_TRACKS as i32,
        stride_bytes: 0,
        track_stride_bytes: 0,
        used_bytes: 4,
        decision: *decision,
    };
    renderer.params_scratch.clear();
    package_params(
        &package,
        renderer.retained_storage,
        output.color_mode,
        sdr_white_scale,
        output.width as i32,
        output.height as i32,
        viewport_rect,
        flutter_surface.as_ref().map_or(0, |surface| surface.width),
        flutter_surface.as_ref().map_or(0, |surface| surface.height),
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        0.0,
        &mut renderer.params_scratch,
    );
    combined_overlay_rects(
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        &mut renderer.overlay_rects_scratch,
    );
    renderer.profiler.last_prepare_us += profile_elapsed_us(prepare_start);
    match renderer.retained_storage {
        STORAGE_BGRA => {
            if renderer.source_texture.is_none() {
                return Err("wgpu-metal retained BGRA source cache is incomplete");
            }
            render_bgra_atlas_with_wgsl(
                renderer,
                &destination_view,
                STORAGE_BGRA,
                output.format,
                output.width,
                output.height,
                overlay_generation,
                flutter_surface.as_ref().map(|surface| &surface.view),
                flutter_surface
                    .as_ref()
                    .map_or(0, |surface| surface.generation),
            )
        }
        STORAGE_YUV => {
            if renderer.package_buffer.is_none() {
                return Err("wgpu-metal retained YUV package source cache is incomplete");
            }
            render_bgra_atlas_with_wgsl(
                renderer,
                &destination_view,
                STORAGE_YUV,
                output.format,
                output.width,
                output.height,
                overlay_generation,
                flutter_surface.as_ref().map(|surface| &surface.view),
                flutter_surface
                    .as_ref()
                    .map_or(0, |surface| surface.generation),
            )
        }
        STORAGE_CV_PIXEL_BUFFER => {
            Err("wgpu-metal direct retained CVPixelBuffer source cache is disabled")
        }
        STORAGE_OUTPUT_ATLAS => {
            if renderer.source_texture.is_none() {
                return Err("wgpu-metal retained output source atlas is incomplete");
            }
            render_bgra_atlas_with_wgsl(
                renderer,
                &destination_view,
                STORAGE_OUTPUT_ATLAS,
                output.format,
                output.width,
                output.height,
                overlay_generation,
                flutter_surface.as_ref().map(|surface| &surface.view),
                flutter_surface
                    .as_ref()
                    .map_or(0, |surface| surface.generation),
            )
        }
        _ => Err("wgpu-metal retained source storage is unsupported"),
    }
}

fn bgra_atlas_for_wgsl(
    source: &[u8],
    package: &PresentFramePackageInfo,
    atlas: &mut Vec<u8>,
) -> Result<(u32, u32), &'static str> {
    if package.used_bytes > source.len() {
        return Err("wgpu-metal BGRA package data is undersized");
    }
    let mut atlas_width = 1u32;
    let mut atlas_height = 1u32;
    for slot in 0..MAX_TRACKS {
        if package.decision.frames[slot].present == 0 {
            continue;
        }
        let source_width = package.decision.source_width[slot];
        let source_height = package.decision.source_height[slot];
        let source_stride = package.decision.y_stride[slot];
        if source_width <= 0 || source_height <= 0 || source_stride < source_width.saturating_mul(4)
        {
            return Err("wgpu-metal BGRA source package metadata is invalid");
        }
        atlas_width = atlas_width.max(source_width as u32);
        atlas_height = atlas_height.max(source_height as u32);
    }
    let row_bytes = (atlas_width as usize)
        .checked_mul(4)
        .ok_or("wgpu-metal BGRA atlas row overflow")?;
    let track_bytes = row_bytes
        .checked_mul(atlas_height as usize)
        .ok_or("wgpu-metal BGRA atlas track overflow")?;
    let atlas_len = track_bytes
        .checked_mul(MAX_TRACKS)
        .ok_or("wgpu-metal BGRA atlas overflow")?;
    atlas.clear();
    atlas.resize(atlas_len, 0);
    for slot in 0..MAX_TRACKS {
        if package.decision.frames[slot].present == 0 {
            continue;
        }
        let source_width = package.decision.source_width[slot] as usize;
        let source_height = package.decision.source_height[slot] as usize;
        let source_stride = package.decision.y_stride[slot] as usize;
        let src_offset = package.decision.y_offset[slot];
        if src_offset < 0 {
            return Err("wgpu-metal BGRA source offset is invalid");
        }
        let src_track = src_offset as usize;
        let source_row_bytes = source_width
            .checked_mul(4)
            .ok_or("wgpu-metal BGRA source row overflow")?;
        let dst_track = track_bytes
            .checked_mul(slot)
            .ok_or("wgpu-metal BGRA atlas offset overflow")?;
        for y in 0..source_height {
            let src_row = src_track
                .checked_add(
                    y.checked_mul(source_stride)
                        .ok_or("wgpu-metal BGRA row offset overflow")?,
                )
                .ok_or("wgpu-metal BGRA row offset overflow")?;
            let dst_row = dst_track
                .checked_add(
                    y.checked_mul(row_bytes)
                        .ok_or("wgpu-metal BGRA atlas row overflow")?,
                )
                .ok_or("wgpu-metal BGRA atlas row overflow")?;
            if src_row + source_row_bytes > package.used_bytes
                || src_row + source_row_bytes > source.len()
            {
                return Err("wgpu-metal BGRA source row is out of bounds");
            }
            atlas[dst_row..dst_row + source_row_bytes]
                .copy_from_slice(&source[src_row..src_row + source_row_bytes]);
        }
    }
    Ok((atlas_width, atlas_height))
}

fn package_storage_bytes(source: &[u8], bytes: &mut Vec<u8>) {
    let padded_len = source.len().max(4).next_multiple_of(4);
    bytes.clear();
    bytes.resize(padded_len, 0);
    bytes[..source.len()].copy_from_slice(source);
}

fn set_dummy_package_storage_bytes(bytes: &mut Vec<u8>) -> bool {
    if bytes.len() == 4 && bytes.iter().all(|byte| *byte == 0) {
        return false;
    }
    bytes.clear();
    bytes.extend_from_slice(&[0, 0, 0, 0]);
    true
}

fn combined_overlay_rects(
    fill: &[OverlayRect],
    line: &[OverlayRect],
    motion: &[OverlayRect],
    rects: &mut Vec<OverlayRect>,
) {
    rects.clear();
    rects.reserve(fill.len() + line.len() + motion.len());
    rects.extend_from_slice(fill);
    rects.extend_from_slice(line);
    rects.extend_from_slice(motion);
    if rects.is_empty() {
        rects.push(OverlayRect::default());
    }
}

fn push_vec4_f32(bytes: &mut Vec<u8>, values: [f32; 4]) {
    for value in values {
        bytes.extend_from_slice(&value.to_ne_bytes());
    }
}

fn push_vec4_i32(bytes: &mut Vec<u8>, values: [i32; 4]) {
    for value in values {
        bytes.extend_from_slice(&value.to_ne_bytes());
    }
}

fn package_params(
    package: &PresentFramePackageInfo,
    storage: i32,
    output_color_mode: i32,
    sdr_white_scale: f32,
    output_width: i32,
    output_height: i32,
    viewport_rect: [f32; 4],
    flutter_width: u32,
    flutter_height: u32,
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_layer_track: f32,
    bytes: &mut Vec<u8>,
) {
    let decision = &package.decision;
    bytes.clear();
    bytes.reserve(31 * 16);
    push_vec4_f32(
        bytes,
        [
            package.width as f32,
            package.height as f32,
            decision.mode as f32,
            decision.track_count.max(1).min(MAX_TRACKS as i32) as f32,
        ],
    );
    push_vec4_f32(
        bytes,
        [decision.split_pos, storage as f32, overlay_layer_track, 0.0],
    );
    push_vec4_f32(bytes, decision.background_color);
    push_vec4_i32(bytes, decision.order);
    push_vec4_f32(bytes, decision.display_offset_x);
    push_vec4_f32(bytes, decision.display_offset_y);
    push_vec4_f32(bytes, decision.inv_display_size_x);
    push_vec4_f32(bytes, decision.inv_display_size_y);
    push_vec4_f32(bytes, decision.view_offset_uv_x);
    push_vec4_f32(bytes, decision.view_offset_uv_y);
    push_vec4_i32(
        bytes,
        [
            decision.frames[0].present,
            decision.frames[1].present,
            decision.frames[2].present,
            decision.frames[3].present,
        ],
    );
    push_vec4_f32(
        bytes,
        [
            decision.source_width[0] as f32,
            decision.source_width[1] as f32,
            decision.source_width[2] as f32,
            decision.source_width[3] as f32,
        ],
    );
    push_vec4_f32(
        bytes,
        [
            decision.source_height[0] as f32,
            decision.source_height[1] as f32,
            decision.source_height[2] as f32,
            decision.source_height[3] as f32,
        ],
    );
    push_vec4_i32(bytes, decision.yuv_format);
    push_vec4_i32(bytes, decision.y_offset);
    push_vec4_i32(bytes, decision.uv_offset);
    push_vec4_i32(bytes, decision.v_offset);
    push_vec4_i32(bytes, decision.y_stride);
    push_vec4_i32(bytes, decision.uv_stride);
    push_vec4_i32(bytes, decision.coded_width);
    push_vec4_i32(bytes, decision.coded_height);
    push_vec4_i32(bytes, decision.color_range);
    push_vec4_i32(bytes, decision.color_matrix);
    push_vec4_i32(
        bytes,
        [
            overlay_fill_rects.len().min(i32::MAX as usize) as i32,
            overlay_line_rects.len().min(i32::MAX as usize) as i32,
            overlay_motion_lines.len().min(i32::MAX as usize) as i32,
            0,
        ],
    );
    push_vec4_i32(bytes, decision.color_transfer);
    push_vec4_i32(bytes, decision.color_primaries);
    push_vec4_i32(bytes, [output_color_mode, 1, 1, 0]);
    push_vec4_f32(bytes, [output_width as f32, output_height as f32, 0.0, 0.0]);
    push_vec4_f32(bytes, viewport_rect);
    push_vec4_f32(
        bytes,
        [flutter_width as f32, flutter_height as f32, 0.0, 0.0],
    );
    push_vec4_f32(bytes, [sdr_white_scale.max(0.0001), 0.0, 0.0, 0.0]);
}

const PARAM_VEC4_BYTES: usize = 16;
const PARAM_PRESENT_VEC: usize = 10;
const PARAM_SOURCE_WIDTH_VEC: usize = 11;
const PARAM_SOURCE_HEIGHT_VEC: usize = 12;
const PARAM_OUTPUT_MODE_VEC: usize = 26;
const MAX_OVERLAY_LAYER_WIDTH: u32 = 1280;
const MAX_OVERLAY_LAYER_HEIGHT: u32 = 720;

fn param_i32(params: &[u8], vec_index: usize, lane: usize) -> i32 {
    let offset = vec_index * PARAM_VEC4_BYTES + lane * core::mem::size_of::<i32>();
    params
        .get(offset..offset + core::mem::size_of::<i32>())
        .and_then(|bytes| bytes.try_into().ok())
        .map(i32::from_ne_bytes)
        .unwrap_or(0)
}

fn param_f32(params: &[u8], vec_index: usize, lane: usize) -> f32 {
    let offset = vec_index * PARAM_VEC4_BYTES + lane * core::mem::size_of::<f32>();
    params
        .get(offset..offset + core::mem::size_of::<f32>())
        .and_then(|bytes| bytes.try_into().ok())
        .map(f32::from_ne_bytes)
        .unwrap_or(0.0)
}

fn write_param_i32(params: &mut [u8], vec_index: usize, lane: usize, value: i32) {
    let offset = vec_index * PARAM_VEC4_BYTES + lane * core::mem::size_of::<i32>();
    if let Some(bytes) = params.get_mut(offset..offset + core::mem::size_of::<i32>()) {
        bytes.copy_from_slice(&value.to_ne_bytes());
    }
}

fn overlay_layer_dimensions(
    params: &mut [u8],
    target_width: u32,
    target_height: u32,
) -> (u32, u32) {
    let mut width = 1u32;
    let mut height = 1u32;
    for track in 0..MAX_TRACKS {
        if param_i32(params, PARAM_PRESENT_VEC, track) == 0 {
            continue;
        }
        width = width.max(param_f32(params, PARAM_SOURCE_WIDTH_VEC, track).ceil() as u32);
        height = height.max(param_f32(params, PARAM_SOURCE_HEIGHT_VEC, track).ceil() as u32);
    }
    width = width
        .max(1)
        .min(target_width.max(1))
        .min(MAX_OVERLAY_LAYER_WIDTH);
    height = height
        .max(1)
        .min(target_height.max(1))
        .min(MAX_OVERLAY_LAYER_HEIGHT);
    write_param_i32(params, PARAM_OUTPUT_MODE_VEC, 1, width as i32);
    write_param_i32(params, PARAM_OUTPUT_MODE_VEC, 2, height as i32);
    (width, height)
}

fn write_source_bgra_atlas(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    cache: &mut Option<CachedSourceTexture>,
    resource_generation: &mut u64,
    width: u32,
    height: u32,
    data: &[u8],
) -> Result<(), &'static str> {
    let layers = MAX_TRACKS as u32;
    let expected_len = (width as usize)
        .checked_mul(height as usize)
        .and_then(|pixels| pixels.checked_mul(4))
        .and_then(|track_bytes| track_bytes.checked_mul(MAX_TRACKS))
        .ok_or("wgpu-metal source atlas size overflow")?;
    if data.len() != expected_len {
        return Err("wgpu-metal source atlas size mismatch");
    }
    let recreate = cache.as_ref().map_or(true, |texture| {
        texture.width != width
            || texture.height != height
            || texture.layers != layers
            || texture.format != wgpu::TextureFormat::Bgra8Unorm
    });
    if recreate {
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("voidplayer-wgpu-source-bgra-atlas"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: layers,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Bgra8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        *resource_generation = resource_generation.wrapping_add(1).max(1);
        *cache = Some(CachedSourceTexture {
            width,
            height,
            layers,
            format: wgpu::TextureFormat::Bgra8Unorm,
            generation: *resource_generation,
            texture,
            view,
        });
    }
    let texture = &cache
        .as_ref()
        .ok_or("wgpu-metal source texture cache is unavailable")?
        .texture;
    queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        data,
        wgpu::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(width * 4),
            rows_per_image: Some(height),
        },
        wgpu::Extent3d {
            width,
            height,
            depth_or_array_layers: layers,
        },
    );
    Ok(())
}

fn ensure_source_output_atlas(
    renderer: &mut WgpuMetalRenderer,
    width: u32,
    height: u32,
    format: wgpu::TextureFormat,
) -> Result<(), &'static str> {
    if width == 0 || height == 0 {
        return Err("wgpu-metal source output atlas dimensions are invalid");
    }
    if !matches!(
        format,
        wgpu::TextureFormat::Bgra8Unorm | wgpu::TextureFormat::Rgba16Float
    ) {
        return Err("wgpu-metal source output atlas format is unsupported");
    }
    let layers = MAX_TRACKS as u32;
    let recreate = renderer.source_texture.as_ref().map_or(true, |texture| {
        texture.width != width
            || texture.height != height
            || texture.layers != layers
            || texture.format != format
    });
    if recreate {
        let texture = renderer.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("voidplayer-wgpu-owned-source-output-atlas"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: layers,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        renderer.resource_generation = renderer.resource_generation.wrapping_add(1).max(1);
        renderer.source_texture = Some(CachedSourceTexture {
            width,
            height,
            layers,
            format,
            generation: renderer.resource_generation,
            texture,
            view,
        });
    }
    Ok(())
}

fn grow_buffer_capacity(size: u64) -> u64 {
    let min_size = size.max(4);
    min_size.checked_next_power_of_two().unwrap_or(min_size)
}

fn write_cached_storage_buffer(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    cache: &mut Option<CachedStorageBuffer>,
    resource_generation: &mut u64,
    bytes: &[u8],
    label: &'static str,
) {
    let size = bytes.len().max(4) as u64;
    let recreate = cache.as_ref().map_or(true, |buffer| buffer.capacity < size);
    if recreate {
        let capacity = grow_buffer_capacity(size);
        *resource_generation = resource_generation.wrapping_add(1).max(1);
        *cache = Some(CachedStorageBuffer {
            capacity,
            generation: *resource_generation,
            buffer: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(label),
                size: capacity,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }),
        });
    }
    let buffer = &cache
        .as_ref()
        .expect("storage buffer cache populated")
        .buffer;
    if bytes.is_empty() {
        queue.write_buffer(buffer, 0, &[0, 0, 0, 0]);
    } else {
        queue.write_buffer(buffer, 0, bytes);
    }
}

fn cached_storage_buffer_needs_capacity(cache: &Option<CachedStorageBuffer>, bytes: &[u8]) -> bool {
    let size = bytes.len().max(4) as u64;
    cache.as_ref().map_or(true, |buffer| buffer.capacity < size)
}

fn write_package_buffer_if_needed(renderer: &mut WgpuMetalRenderer) {
    if renderer.package_buffer_dirty
        || cached_storage_buffer_needs_capacity(
            &renderer.package_buffer,
            &renderer.package_bytes_scratch,
        )
    {
        write_cached_storage_buffer(
            &renderer.device,
            &renderer.queue,
            &mut renderer.package_buffer,
            &mut renderer.resource_generation,
            &renderer.package_bytes_scratch,
            "voidplayer-wgpu-package-bytes",
        );
        renderer.package_buffer_dirty = false;
        renderer.profiler.package_buffer_write_count += 1;
    }
}

fn overlay_rect_bytes(rects: &[OverlayRect]) -> &[u8] {
    let len = core::mem::size_of_val(rects);
    unsafe { core::slice::from_raw_parts(rects.as_ptr().cast::<u8>(), len) }
}

fn write_overlay_layer_track_params(base: &[u8], track: usize, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(base);
    let split_z_offset = 16 + 8;
    if out.len() >= split_z_offset + core::mem::size_of::<f32>() {
        out[split_z_offset..split_z_offset + 4].copy_from_slice(&(track as f32).to_ne_bytes());
    }
}

fn encode_overlay_layer_texture(
    renderer: &mut WgpuMetalRenderer,
    width: u32,
    height: u32,
    overlay_generation: u64,
    encoder: &mut wgpu::CommandEncoder,
) -> Result<(), &'static str> {
    let profile_start = Instant::now();
    let fill_count = renderer
        .params_scratch
        .get((23 * 16)..(23 * 16 + 4))
        .and_then(|bytes| bytes.try_into().ok())
        .map(i32::from_ne_bytes)
        .unwrap_or(0)
        .max(0) as usize;
    let line_count = renderer
        .params_scratch
        .get((23 * 16 + 4)..(23 * 16 + 8))
        .and_then(|bytes| bytes.try_into().ok())
        .map(i32::from_ne_bytes)
        .unwrap_or(0)
        .max(0) as usize;
    let motion_count = renderer
        .params_scratch
        .get((23 * 16 + 8)..(23 * 16 + 12))
        .and_then(|bytes| bytes.try_into().ok())
        .map(i32::from_ne_bytes)
        .unwrap_or(0)
        .max(0) as usize;
    let layers = MAX_TRACKS as u32;
    let cache_hit = renderer
        .overlay_layer_texture
        .as_ref()
        .map_or(false, |cache| {
            cache.width == width
                && cache.height == height
                && cache.layers == layers
                && cache.overlay_generation == overlay_generation
                && cache.fill_count == fill_count
                && cache.line_count == line_count
                && cache.motion_count == motion_count
        });
    if cache_hit
        && (overlay_generation != 0 || (fill_count == 0 && line_count == 0 && motion_count == 0))
    {
        renderer.profiler.overlay_layer_reuse_count += 1;
        renderer.profiler.last_overlay_encode_us += profile_elapsed_us(profile_start);
        return Ok(());
    }
    let recreate = renderer
        .overlay_layer_texture
        .as_ref()
        .map_or(true, |cache| {
            cache.width != width || cache.height != height || cache.layers != layers
        });
    if recreate {
        let texture = renderer.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("voidplayer-wgpu-overlay-layer-array"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: layers,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        renderer.resource_generation = renderer.resource_generation.wrapping_add(1).max(1);
        renderer.overlay_layer_texture = Some(CachedOverlayLayerTexture {
            width,
            height,
            layers,
            overlay_generation,
            fill_count,
            line_count,
            motion_count,
            resource_generation: renderer.resource_generation,
            texture,
            view,
        });
    }
    renderer.profiler.overlay_layer_rebuild_count += 1;
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.overlay_buffer,
        &mut renderer.resource_generation,
        overlay_rect_bytes(&renderer.overlay_rects_scratch),
        "voidplayer-wgpu-overlay-combined-rects",
    );
    renderer.profiler.overlay_buffer_write_count += 1;
    let source_view = &renderer.dummy_bgra_array_view;
    let dummy_y_view = &renderer.dummy_y_view;
    let dummy_uv_view = &renderer.dummy_uv_view;
    let package_generation = renderer
        .package_buffer
        .as_ref()
        .ok_or("wgpu-metal package buffer cache is unavailable")?
        .generation;
    let overlay_buffer_generation = renderer
        .overlay_buffer
        .as_ref()
        .ok_or("wgpu-metal overlay buffer cache is unavailable")?
        .generation;
    let overlay_texture_generation = renderer
        .overlay_layer_texture
        .as_ref()
        .ok_or("wgpu-metal overlay layer texture is unavailable")?
        .resource_generation;
    let final_params = renderer.params_scratch.clone();
    let mut layer_params = Vec::with_capacity(final_params.len());
    for track in 0..MAX_TRACKS {
        if param_i32(&final_params, PARAM_PRESENT_VEC, track) == 0 {
            continue;
        }
        write_overlay_layer_track_params(&final_params, track, &mut layer_params);
        write_cached_storage_buffer(
            &renderer.device,
            &renderer.queue,
            &mut renderer.overlay_layer_params_buffers[track],
            &mut renderer.resource_generation,
            &layer_params,
            "voidplayer-wgpu-overlay-layer-params",
        );
        renderer.profiler.params_buffer_write_count += 1;
        let params_generation = renderer.overlay_layer_params_buffers[track]
            .as_ref()
            .ok_or("wgpu-metal overlay layer params buffer cache is unavailable")?
            .generation;
        let recreate_bind_group =
            renderer.overlay_layer_bind_groups[track]
                .as_ref()
                .map_or(true, |cache| {
                    cache.params_generation != params_generation
                        || cache.package_generation != package_generation
                        || cache.overlay_generation != overlay_buffer_generation
                });
        if recreate_bind_group {
            let params_buffer = &renderer.overlay_layer_params_buffers[track]
                .as_ref()
                .ok_or("wgpu-metal overlay layer params buffer cache is unavailable")?
                .buffer;
            let package_buffer = &renderer
                .package_buffer
                .as_ref()
                .ok_or("wgpu-metal package buffer cache is unavailable")?
                .buffer;
            let overlay_buffer = &renderer
                .overlay_buffer
                .as_ref()
                .ok_or("wgpu-metal overlay buffer cache is unavailable")?
                .buffer;
            let bind_group = renderer
                .device
                .create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("voidplayer-wgpu-overlay-layer-bind-group"),
                    layout: &renderer.bind_group_layout,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: params_buffer.as_entire_binding(),
                        },
                        wgpu::BindGroupEntry {
                            binding: 1,
                            resource: wgpu::BindingResource::TextureView(source_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 2,
                            resource: wgpu::BindingResource::Sampler(&renderer.sampler),
                        },
                        wgpu::BindGroupEntry {
                            binding: 3,
                            resource: package_buffer.as_entire_binding(),
                        },
                        wgpu::BindGroupEntry {
                            binding: 4,
                            resource: overlay_buffer.as_entire_binding(),
                        },
                        wgpu::BindGroupEntry {
                            binding: 5,
                            resource: wgpu::BindingResource::TextureView(dummy_y_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 6,
                            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 7,
                            resource: wgpu::BindingResource::TextureView(dummy_y_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 8,
                            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 9,
                            resource: wgpu::BindingResource::TextureView(dummy_y_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 10,
                            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 11,
                            resource: wgpu::BindingResource::TextureView(dummy_y_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 12,
                            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 13,
                            resource: wgpu::BindingResource::TextureView(source_view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 14,
                            resource: wgpu::BindingResource::TextureView(
                                &renderer.dummy_flutter_view,
                            ),
                        },
                    ],
                });
            renderer.overlay_layer_bind_groups[track] = Some(CachedOverlayLayerBindGroup {
                params_generation,
                package_generation,
                overlay_generation: overlay_buffer_generation,
                bind_group,
            });
            renderer.profiler.overlay_bind_group_create_count += 1;
        }
        let recreate_layer_view = renderer.overlay_layer_views[track]
            .as_ref()
            .map_or(true, |cache| {
                cache.texture_generation != overlay_texture_generation
            });
        if recreate_layer_view {
            let overlay_texture = &renderer
                .overlay_layer_texture
                .as_ref()
                .ok_or("wgpu-metal overlay layer texture is unavailable")?
                .texture;
            let view = overlay_texture.create_view(&wgpu::TextureViewDescriptor {
                label: Some("voidplayer-wgpu-overlay-layer-slice-view"),
                format: Some(wgpu::TextureFormat::Rgba8Unorm),
                dimension: Some(wgpu::TextureViewDimension::D2),
                usage: Some(wgpu::TextureUsages::RENDER_ATTACHMENT),
                aspect: wgpu::TextureAspect::All,
                base_mip_level: 0,
                mip_level_count: Some(1),
                base_array_layer: track as u32,
                array_layer_count: Some(1),
            });
            renderer.overlay_layer_views[track] = Some(CachedOverlayLayerView {
                texture_generation: overlay_texture_generation,
                view,
            });
        }
        let bind_group = &renderer.overlay_layer_bind_groups[track]
            .as_ref()
            .ok_or("wgpu-metal overlay layer bind group cache is unavailable")?
            .bind_group;
        let layer_view = &renderer.overlay_layer_views[track]
            .as_ref()
            .ok_or("wgpu-metal overlay layer view cache is unavailable")?
            .view;
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("voidplayer-wgpu-overlay-layer-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: layer_view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_bind_group(0, bind_group, &[]);
            pass.set_viewport(0.0, 0.0, width as f32, height as f32, 0.0, 1.0);
            let primitive_vertices = fill_count
                .checked_mul(6)
                .and_then(|count| count.checked_add(line_count.checked_mul(4 * 6 * 2)?))
                .and_then(|count| count.checked_add(motion_count.checked_mul(6)?))
                .and_then(|count| u32::try_from(count).ok())
                .ok_or("wgpu-metal overlay primitive vertex count is too large")?;
            if primitive_vertices > 0 {
                pass.set_pipeline(&renderer.overlay_primitive_pipeline);
                pass.draw(0..primitive_vertices, 0..1);
            }
        }
    }
    if let Some(cache) = renderer.overlay_layer_texture.as_mut() {
        cache.overlay_generation = overlay_generation;
        cache.fill_count = fill_count;
        cache.line_count = line_count;
        cache.motion_count = motion_count;
    }
    renderer.profiler.last_overlay_encode_us += profile_elapsed_us(profile_start);
    Ok(())
}

fn composite_bind_group_entries<'a>(
    params_buffer: &'a wgpu::Buffer,
    source_view: &'a wgpu::TextureView,
    sampler: &'a wgpu::Sampler,
    package_buffer: &'a wgpu::Buffer,
    overlay_buffer: &'a wgpu::Buffer,
    dummy_y_view: &'a wgpu::TextureView,
    dummy_uv_view: &'a wgpu::TextureView,
    overlay_layer_texture: &'a wgpu::TextureView,
    flutter_view: &'a wgpu::TextureView,
) -> [wgpu::BindGroupEntry<'a>; 15] {
    [
        wgpu::BindGroupEntry {
            binding: 0,
            resource: params_buffer.as_entire_binding(),
        },
        wgpu::BindGroupEntry {
            binding: 1,
            resource: wgpu::BindingResource::TextureView(source_view),
        },
        wgpu::BindGroupEntry {
            binding: 2,
            resource: wgpu::BindingResource::Sampler(sampler),
        },
        wgpu::BindGroupEntry {
            binding: 3,
            resource: package_buffer.as_entire_binding(),
        },
        wgpu::BindGroupEntry {
            binding: 4,
            resource: overlay_buffer.as_entire_binding(),
        },
        wgpu::BindGroupEntry {
            binding: 5,
            resource: wgpu::BindingResource::TextureView(dummy_y_view),
        },
        wgpu::BindGroupEntry {
            binding: 6,
            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
        },
        wgpu::BindGroupEntry {
            binding: 7,
            resource: wgpu::BindingResource::TextureView(dummy_y_view),
        },
        wgpu::BindGroupEntry {
            binding: 8,
            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
        },
        wgpu::BindGroupEntry {
            binding: 9,
            resource: wgpu::BindingResource::TextureView(dummy_y_view),
        },
        wgpu::BindGroupEntry {
            binding: 10,
            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
        },
        wgpu::BindGroupEntry {
            binding: 11,
            resource: wgpu::BindingResource::TextureView(dummy_y_view),
        },
        wgpu::BindGroupEntry {
            binding: 12,
            resource: wgpu::BindingResource::TextureView(dummy_uv_view),
        },
        wgpu::BindGroupEntry {
            binding: 13,
            resource: wgpu::BindingResource::TextureView(overlay_layer_texture),
        },
        wgpu::BindGroupEntry {
            binding: 14,
            resource: wgpu::BindingResource::TextureView(flutter_view),
        },
    ]
}

fn render_bgra_atlas_with_wgsl(
    renderer: &mut WgpuMetalRenderer,
    destination_view: &wgpu::TextureView,
    source_storage: i32,
    output_format: wgpu::TextureFormat,
    width: u32,
    height: u32,
    overlay_generation: u64,
    flutter_view: Option<&wgpu::TextureView>,
    flutter_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    write_package_buffer_if_needed(renderer);
    let mut encoder = renderer
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("voidplayer-wgpu-composite-encoder"),
        });
    let (overlay_width, overlay_height) =
        overlay_layer_dimensions(&mut renderer.params_scratch, width, height);
    encode_overlay_layer_texture(
        renderer,
        overlay_width,
        overlay_height,
        overlay_generation,
        &mut encoder,
    )?;
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.params_buffer,
        &mut renderer.resource_generation,
        &renderer.params_scratch,
        "voidplayer-wgpu-composite-params",
    );
    renderer.profiler.params_buffer_write_count += 1;

    let params_buffer = &renderer
        .params_buffer
        .as_ref()
        .ok_or("wgpu-metal params buffer cache is unavailable")?
        .buffer;
    let package_buffer = &renderer
        .package_buffer
        .as_ref()
        .ok_or("wgpu-metal package buffer cache is unavailable")?
        .buffer;
    let overlay_buffer = &renderer
        .overlay_buffer
        .as_ref()
        .ok_or("wgpu-metal overlay buffer cache is unavailable")?
        .buffer;
    let overlay_layer_texture = &renderer
        .overlay_layer_texture
        .as_ref()
        .ok_or("wgpu-metal overlay layer texture is unavailable")?
        .view;
    let overlay_layer_generation = renderer
        .overlay_layer_texture
        .as_ref()
        .ok_or("wgpu-metal overlay layer texture is unavailable")?
        .resource_generation;
    let source_view: &wgpu::TextureView =
        if source_storage == STORAGE_BGRA || source_storage == STORAGE_OUTPUT_ATLAS {
            &renderer
                .source_texture
                .as_ref()
                .ok_or("wgpu-metal source texture cache is unavailable")?
                .view
        } else {
            &renderer.dummy_bgra_array_view
        };
    let source_generation =
        if source_storage == STORAGE_BGRA || source_storage == STORAGE_OUTPUT_ATLAS {
            renderer
                .source_texture
                .as_ref()
                .ok_or("wgpu-metal source texture cache is unavailable")?
                .generation
        } else {
            0
        };
    let dummy_y_view = &renderer.dummy_y_view;
    let dummy_uv_view = &renderer.dummy_uv_view;
    let flutter_view = flutter_view.unwrap_or(&renderer.dummy_flutter_view);
    let bind_group_key = BindGroupKey {
        source_storage,
        params_generation: renderer
            .params_buffer
            .as_ref()
            .ok_or("wgpu-metal params buffer cache is unavailable")?
            .generation,
        package_generation: renderer
            .package_buffer
            .as_ref()
            .ok_or("wgpu-metal package buffer cache is unavailable")?
            .generation,
        overlay_generation: renderer
            .overlay_buffer
            .as_ref()
            .ok_or("wgpu-metal overlay buffer cache is unavailable")?
            .generation,
        source_generation,
        overlay_layer_generation,
        flutter_generation,
    };
    let bind_group_start = Instant::now();
    let bind_group = if let Some(cache) = renderer.generic_bind_group.as_ref() {
        if cache.key == bind_group_key {
            cache.bind_group.clone()
        } else {
            let bind_group = renderer
                .device
                .create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("voidplayer-wgpu-composite-bind-group"),
                    layout: &renderer.bind_group_layout,
                    entries: &composite_bind_group_entries(
                        params_buffer,
                        source_view,
                        &renderer.sampler,
                        package_buffer,
                        overlay_buffer,
                        dummy_y_view,
                        dummy_uv_view,
                        overlay_layer_texture,
                        flutter_view,
                    ),
                });
            renderer.generic_bind_group = Some(CachedBindGroup {
                key: bind_group_key,
                bind_group: bind_group.clone(),
            });
            renderer.profiler.final_bind_group_create_count += 1;
            bind_group
        }
    } else {
        let bind_group = renderer
            .device
            .create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("voidplayer-wgpu-composite-bind-group"),
                layout: &renderer.bind_group_layout,
                entries: &composite_bind_group_entries(
                    params_buffer,
                    source_view,
                    &renderer.sampler,
                    package_buffer,
                    overlay_buffer,
                    dummy_y_view,
                    dummy_uv_view,
                    overlay_layer_texture,
                    flutter_view,
                ),
            });
        renderer.generic_bind_group = Some(CachedBindGroup {
            key: bind_group_key,
            bind_group: bind_group.clone(),
        });
        renderer.profiler.final_bind_group_create_count += 1;
        bind_group
    };
    renderer.profiler.last_bind_group_us += profile_elapsed_us(bind_group_start);
    let pipeline = composite_pipeline_for_output(renderer, output_format)?;
    let pass_start = Instant::now();
    {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("voidplayer-wgpu-composite-pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: destination_view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, &bind_group, &[]);
        pass.set_viewport(0.0, 0.0, width as f32, height as f32, 0.0, 1.0);
        pass.draw(0..3, 0..1);
    }
    {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("voidplayer-wgpu-flutter-surface-pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: destination_view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Load,
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        let pipeline = flutter_pipeline_for_output(renderer, output_format)?;
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, &bind_group, &[]);
        pass.set_viewport(0.0, 0.0, width as f32, height as f32, 0.0, 1.0);
        pass.draw(0..3, 0..1);
    }
    renderer.profiler.last_pass_encode_us += profile_elapsed_us(pass_start);
    let submit_start = Instant::now();
    let submission = renderer.queue.submit(std::iter::once(encoder.finish()));
    renderer.profiler.last_submit_us += profile_elapsed_us(submit_start);
    renderer.profiler.submit_count += 1;
    Ok(submission)
}

fn source_output_atlas_dimensions(
    decision: &PresentDecisionInfo,
) -> Result<(u32, u32), &'static str> {
    let mut width = 1u32;
    let mut height = 1u32;
    let mut present = false;
    for slot in 0..MAX_TRACKS {
        if decision.frames[slot].present == 0 {
            continue;
        }
        if decision.source_width[slot] <= 0 || decision.source_height[slot] <= 0 {
            return Err("wgpu-metal source output atlas frame dimensions are invalid");
        }
        present = true;
        width = width.max(decision.source_width[slot] as u32);
        height = height.max(decision.source_height[slot] as u32);
    }
    if !present {
        return Err("wgpu-metal source output atlas has no presentable frame");
    }
    Ok((width, height))
}

fn cv_source_bake_params(
    frame_set: &CVPixelBufferPresentFrameSet,
    track: usize,
    output_color_mode: i32,
    sdr_white_scale: f32,
    output_width: i32,
    output_height: i32,
    bytes: &mut Vec<u8>,
) {
    let mut decision = frame_set.decision;
    decision.mode = 0;
    decision.track_count = 1;
    decision.split_pos = 0.5;
    decision.order = [track as i32, 1, 2, 3];
    for slot in 0..MAX_TRACKS {
        decision.frames[slot].present = if slot == track { 1 } else { 0 };
        decision.display_offset_x[slot] = 0.0;
        decision.display_offset_y[slot] = 0.0;
        decision.inv_display_size_x[slot] = 1.0;
        decision.inv_display_size_y[slot] = 1.0;
        decision.view_offset_uv_x[slot] = 0.0;
        decision.view_offset_uv_y[slot] = 0.0;
    }
    decision.background_color = [0.0, 0.0, 0.0, 0.0];
    let package = PresentFramePackageInfo {
        storage: STORAGE_CV_PIXEL_BUFFER,
        width: output_width,
        height: output_height,
        max_track_slots: MAX_TRACKS as i32,
        stride_bytes: 0,
        track_stride_bytes: 0,
        used_bytes: 4,
        decision,
    };
    package_params(
        &package,
        STORAGE_CV_PIXEL_BUFFER,
        output_color_mode,
        sdr_white_scale,
        output_width,
        output_height,
        [
            0.0,
            0.0,
            output_width.max(1) as f32,
            output_height.max(1) as f32,
        ],
        0,
        0,
        &[],
        &[],
        &[],
        0.0,
        bytes,
    );
    write_param_i32(bytes, PARAM_OUTPUT_MODE_VEC, 3, 1);
}

fn encode_cv_source_output_atlas(
    renderer: &mut WgpuMetalRenderer,
    encoder: &mut wgpu::CommandEncoder,
    source_y_textures: &[Option<wgpu::TextureView>; MAX_TRACKS],
    source_uv_textures: &[Option<wgpu::TextureView>; MAX_TRACKS],
    frame_set: &CVPixelBufferPresentFrameSet,
    output: OutputTarget,
    sdr_white_scale: f32,
) -> Result<(), &'static str> {
    let (atlas_width, atlas_height) = source_output_atlas_dimensions(&frame_set.decision)?;
    ensure_source_output_atlas(renderer, atlas_width, atlas_height, output.format)?;

    if renderer.package_bytes_scratch.is_empty() {
        renderer.package_buffer_dirty |=
            set_dummy_package_storage_bytes(&mut renderer.package_bytes_scratch);
    }
    write_package_buffer_if_needed(renderer);
    let empty_overlay = [OverlayRect::default()];
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.overlay_buffer,
        &mut renderer.resource_generation,
        overlay_rect_bytes(&empty_overlay),
        "voidplayer-wgpu-source-bake-empty-overlay",
    );

    let package_buffer = &renderer
        .package_buffer
        .as_ref()
        .ok_or("wgpu-metal package buffer cache is unavailable")?
        .buffer;
    let overlay_buffer = &renderer
        .overlay_buffer
        .as_ref()
        .ok_or("wgpu-metal overlay buffer cache is unavailable")?
        .buffer;
    let source_view = &renderer.dummy_bgra_array_view;
    let overlay_layer_texture = &renderer.dummy_bgra_array_view;
    let flutter_view = &renderer.dummy_flutter_view;
    let y_view = |slot: usize| {
        source_y_textures[slot]
            .as_ref()
            .unwrap_or(&renderer.dummy_y_view)
    };
    let uv_view = |slot: usize| {
        source_uv_textures[slot]
            .as_ref()
            .unwrap_or(&renderer.dummy_uv_view)
    };
    let pipeline = composite_pipeline_for_output(renderer, output.format)?;

    for track in 0..MAX_TRACKS {
        if frame_set.decision.frames[track].present == 0 {
            continue;
        }
        let source_width = frame_set.decision.source_width[track].max(1) as u32;
        let source_height = frame_set.decision.source_height[track].max(1) as u32;
        let mut bake_params = Vec::new();
        cv_source_bake_params(
            frame_set,
            track,
            output.color_mode,
            sdr_white_scale,
            source_width as i32,
            source_height as i32,
            &mut bake_params,
        );
        let params_buffer = renderer.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("voidplayer-wgpu-source-bake-params"),
            size: bake_params.len().max(4) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        renderer.queue.write_buffer(&params_buffer, 0, &bake_params);
        let bind_group = renderer
            .device
            .create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("voidplayer-wgpu-source-bake-bind-group"),
                layout: &renderer.bind_group_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: params_buffer.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: wgpu::BindingResource::TextureView(source_view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: wgpu::BindingResource::Sampler(&renderer.sampler),
                    },
                    wgpu::BindGroupEntry {
                        binding: 3,
                        resource: package_buffer.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 4,
                        resource: overlay_buffer.as_entire_binding(),
                    },
                    wgpu::BindGroupEntry {
                        binding: 5,
                        resource: wgpu::BindingResource::TextureView(y_view(0)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 6,
                        resource: wgpu::BindingResource::TextureView(uv_view(0)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 7,
                        resource: wgpu::BindingResource::TextureView(y_view(1)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 8,
                        resource: wgpu::BindingResource::TextureView(uv_view(1)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 9,
                        resource: wgpu::BindingResource::TextureView(y_view(2)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 10,
                        resource: wgpu::BindingResource::TextureView(uv_view(2)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 11,
                        resource: wgpu::BindingResource::TextureView(y_view(3)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 12,
                        resource: wgpu::BindingResource::TextureView(uv_view(3)),
                    },
                    wgpu::BindGroupEntry {
                        binding: 13,
                        resource: wgpu::BindingResource::TextureView(overlay_layer_texture),
                    },
                    wgpu::BindGroupEntry {
                        binding: 14,
                        resource: wgpu::BindingResource::TextureView(flutter_view),
                    },
                ],
            });
        let layer_view = renderer
            .source_texture
            .as_ref()
            .ok_or("wgpu-metal source output atlas is unavailable")?
            .texture
            .create_view(&wgpu::TextureViewDescriptor {
                label: Some("voidplayer-wgpu-source-bake-layer-view"),
                format: Some(output.format),
                dimension: Some(wgpu::TextureViewDimension::D2),
                usage: Some(wgpu::TextureUsages::RENDER_ATTACHMENT),
                aspect: wgpu::TextureAspect::All,
                base_mip_level: 0,
                mip_level_count: Some(1),
                base_array_layer: track as u32,
                array_layer_count: Some(1),
            });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("voidplayer-wgpu-source-bake-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &layer_view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &bind_group, &[]);
            pass.set_viewport(
                0.0,
                0.0,
                source_width as f32,
                source_height as f32,
                0.0,
                1.0,
            );
            pass.draw(0..3, 0..1);
        }
    }
    if let Some(cache) = renderer.source_texture.as_mut() {
        renderer.resource_generation = renderer.resource_generation.wrapping_add(1).max(1);
        cache.generation = renderer.resource_generation;
    }
    Ok(())
}

fn render_cv_pixel_buffer_frame_set_with_wgsl(
    renderer: &mut WgpuMetalRenderer,
    destination_view: &wgpu::TextureView,
    source_y_textures: &[Option<wgpu::TextureView>; MAX_TRACKS],
    source_uv_textures: &[Option<wgpu::TextureView>; MAX_TRACKS],
    frame_set: &CVPixelBufferPresentFrameSet,
    output_color_mode: i32,
    output_format: wgpu::TextureFormat,
    width: u32,
    height: u32,
    overlay_generation: u64,
    flutter_view: Option<&wgpu::TextureView>,
    flutter_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    let mut source_encoder =
        renderer
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("voidplayer-wgpu-cvpixelbuffer-source-bake-encoder"),
            });
    encode_cv_source_output_atlas(
        renderer,
        &mut source_encoder,
        source_y_textures,
        source_uv_textures,
        frame_set,
        OutputTarget {
            format: output_format,
            color_mode: output_color_mode,
            width,
            height,
        },
        1.0,
    )?;
    let submit_start = Instant::now();
    let _source_submission = renderer
        .queue
        .submit(std::iter::once(source_encoder.finish()));
    renderer.profiler.last_submit_us += profile_elapsed_us(submit_start);
    renderer.profiler.submit_count += 1;
    render_bgra_atlas_with_wgsl(
        renderer,
        destination_view,
        STORAGE_OUTPUT_ATLAS,
        output_format,
        width,
        height,
        overlay_generation,
        flutter_view,
        flutter_generation,
    )
}
