#![deny(unsafe_op_in_unsafe_fn)]

pub mod color;
pub mod import;
pub mod layout;
pub mod overlay;

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2_metal::{MTLTexture, MTLTextureType};
use overlay::OverlayRect;
use std::sync::mpsc;
use std::thread::{self, JoinHandle};

pub const ABI_VERSION: i32 = 6;
const MAX_TRACKS: usize = 4;
const STORAGE_NONE: i32 = 0;
const STORAGE_YUV: i32 = 1;
const STORAGE_BGRA: i32 = 2;
const STORAGE_CV_PIXEL_BUFFER: i32 = 3;
const OUTPUT_FORMAT_BGRA8_UNORM: i32 = 1;
const OUTPUT_FORMAT_RGBA16_FLOAT: i32 = 2;
const OUTPUT_COLOR_MODE_SDR: i32 = 1;
const OUTPUT_COLOR_MODE_EDR: i32 = 2;

#[repr(C)]
pub struct WgpuMetalRenderRequest {
    pub destination_mtl_texture: *mut core::ffi::c_void,
    pub output_format: i32,
    pub output_color_mode: i32,
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
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
}

#[repr(C)]
pub struct WgpuMetalCVPixelBufferRenderRequest {
    pub destination_mtl_texture: *mut core::ffi::c_void,
    pub output_format: i32,
    pub output_color_mode: i32,
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
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
}

#[repr(C)]
pub struct WgpuMetalRetainedCompositeRequest {
    pub destination_mtl_texture: *mut core::ffi::c_void,
    pub output_format: i32,
    pub output_color_mode: i32,
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
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
}

pub type WgpuMetalAsyncCompletionCallback =
    extern "C" fn(*mut core::ffi::c_void, i32);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgpuMetalAsyncCompletion {
    pub callback: Option<WgpuMetalAsyncCompletionCallback>,
    pub user_data: *mut core::ffi::c_void,
}

struct CompletionJob {
    submission: wgpu::SubmissionIndex,
    callback: WgpuMetalAsyncCompletionCallback,
    user_data: usize,
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
        (OUTPUT_FORMAT_RGBA16_FLOAT, OUTPUT_COLOR_MODE_EDR) => Ok(OutputTarget {
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

pub fn render_metal_package_with_renderer_async(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalRenderRequest,
    completion: WgpuMetalAsyncCompletion,
) -> Result<(), &'static str> {
    let submission = submit_metal_package_with_renderer(renderer, request)?;
    renderer.submit_completion(submission, completion)
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
    if package.width <= 0
        || package.height <= 0
        || package.width != request.width
        || package.height != request.height
    {
        return Err("wgpu-metal package dimensions do not match destination");
    }
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
        source,
        package,
        output,
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

pub fn render_metal_cv_pixel_buffer_frame_set_with_renderer_async(
    renderer: &mut WgpuMetalRenderer,
    request: &WgpuMetalCVPixelBufferRenderRequest,
    completion: WgpuMetalAsyncCompletion,
) -> Result<(), &'static str> {
    let submission = submit_metal_cv_pixel_buffer_frame_set_with_renderer(renderer, request)?;
    renderer.submit_completion(submission, completion)
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
        &request.source_y_mtl_textures,
        &request.source_uv_mtl_textures,
        frame_set,
        output,
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
    let submission = submit_metal_retained_source_with_renderer(renderer, request)?;
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
        decision,
        output,
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
    required_features: wgpu::Features,
    device: wgpu::Device,
    queue: wgpu::Queue,
    sampler: wgpu::Sampler,
    bind_group_layout: wgpu::BindGroupLayout,
    bgra8_pipeline: wgpu::RenderPipeline,
    rgba16_float_pipeline: wgpu::RenderPipeline,
    overlay_pipeline: wgpu::RenderPipeline,
    _dummy_bgra_array_texture: wgpu::Texture,
    dummy_bgra_array_view: wgpu::TextureView,
    _dummy_y_texture: wgpu::Texture,
    dummy_y_view: wgpu::TextureView,
    _dummy_uv_texture: wgpu::Texture,
    dummy_uv_view: wgpu::TextureView,
    overlay_layer_texture: Option<CachedOverlayLayerTexture>,
    source_texture: Option<CachedSourceTexture>,
    params_buffer: Option<CachedStorageBuffer>,
    package_buffer: Option<CachedStorageBuffer>,
    overlay_buffer: Option<CachedStorageBuffer>,
    overlay_layer_params_buffers: [Option<CachedStorageBuffer>; MAX_TRACKS],
    retained_storage: i32,
    retained_cv_y_textures: [Option<wgpu::Texture>; MAX_TRACKS],
    retained_cv_uv_textures: [Option<wgpu::Texture>; MAX_TRACKS],
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
        self.required_features
            .contains(wgpu::Features::TEXTURE_FORMAT_16BIT_NORM)
    }

    fn submit_completion(
        &self,
        submission: wgpu::SubmissionIndex,
        completion: WgpuMetalAsyncCompletion,
    ) -> Result<(), &'static str> {
        let callback = completion
            .callback
            .ok_or("wgpu-metal async completion callback is null")?;
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
    texture: wgpu::Texture,
    view: wgpu::TextureView,
}

struct CachedStorageBuffer {
    capacity: u64,
    buffer: wgpu::Buffer,
}

struct CachedOverlayLayerTexture {
    width: u32,
    height: u32,
    layers: u32,
    generation: u64,
    fill_count: usize,
    line_count: usize,
    motion_count: usize,
    texture: wgpu::Texture,
    view: wgpu::TextureView,
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

impl WgpuMetalRenderer {
    pub fn new() -> Result<Self, &'static str> {
        pollster::block_on(Self::new_async())
    }

    async fn new_async() -> Result<Self, &'static str> {
        let mut instance_desc = wgpu::InstanceDescriptor::new_without_display_handle();
        instance_desc.backends = wgpu::Backends::METAL;
        let instance = wgpu::Instance::new(instance_desc);
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions::default())
            .await
            .map_err(|_| "wgpu-metal failed to create Metal adapter")?;
        let required_features = wgpu::Features::TEXTURE_FORMAT_16BIT_NORM;
        if !adapter.features().contains(required_features) {
            return Err("wgpu-metal Metal adapter lacks 16-bit normalized texture support");
        }
        let adapter_limits = adapter.limits();
        let mut required_limits = wgpu::Limits::downlevel_defaults();
        required_limits.max_texture_dimension_2d = adapter_limits.max_texture_dimension_2d;
        let adapter_info = adapter.get_info();
        let (device, queue) = adapter
            .request_device(&wgpu::DeviceDescriptor {
                label: Some("voidplayer-wgpu-metal"),
                required_features,
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
                    visibility: wgpu::ShaderStages::FRAGMENT,
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
                    visibility: wgpu::ShaderStages::FRAGMENT,
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
        let overlay_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("voidplayer-wgpu-overlay-layer-pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_overlay_layer"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba8Unorm,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });
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
            required_features,
            device,
            queue,
            sampler,
            bind_group_layout,
            bgra8_pipeline,
            rgba16_float_pipeline,
            overlay_pipeline,
            _dummy_bgra_array_texture: dummy_bgra_array_texture,
            dummy_bgra_array_view,
            _dummy_y_texture: dummy_y_texture,
            dummy_y_view,
            _dummy_uv_texture: dummy_uv_texture,
            dummy_uv_view,
            overlay_layer_texture: None,
            source_texture: None,
            params_buffer: None,
            package_buffer: None,
            overlay_buffer: None,
            overlay_layer_params_buffers: std::array::from_fn(|_| None),
            retained_storage: STORAGE_NONE,
            retained_cv_y_textures: std::array::from_fn(|_| None),
            retained_cv_uv_textures: std::array::from_fn(|_| None),
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

fn import_metal_texture_2d(
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

fn render_package_to_metal_destination(
    renderer: &mut WgpuMetalRenderer,
    destination_mtl_texture: *mut core::ffi::c_void,
    source: &[u8],
    package: &PresentFramePackageInfo,
    output: OutputTarget,
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    let destination_texture = import_metal_texture_2d(
        &renderer.device,
        destination_mtl_texture,
        output.format,
        output.width,
        output.height,
        wgpu::TextureUsages::RENDER_ATTACHMENT,
        "voidplayer-imported-cvpixelbuffer-destination",
    )?;

    renderer.params_scratch.clear();
    if package.storage == STORAGE_BGRA {
        bgra_atlas_for_wgsl(source, package, &mut renderer.source_bgra_scratch)?;
        package_params(
            package,
            STORAGE_BGRA,
            output.color_mode,
            overlay_fill_rects,
            overlay_line_rects,
            overlay_motion_lines,
            0.0,
            &mut renderer.params_scratch,
        );
        dummy_package_storage_bytes(&mut renderer.package_bytes_scratch);
        write_source_bgra_atlas(
            &renderer.device,
            &renderer.queue,
            &mut renderer.source_texture,
            output.width,
            output.height,
            &renderer.source_bgra_scratch,
        )?;
    } else {
        package_params(
            package,
            STORAGE_YUV,
            output.color_mode,
            overlay_fill_rects,
            overlay_line_rects,
            overlay_motion_lines,
            0.0,
            &mut renderer.params_scratch,
        );
        package_storage_bytes(source, &mut renderer.package_bytes_scratch);
    }
    combined_overlay_rects(
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        &mut renderer.overlay_rects_scratch,
    );

    let submission = render_bgra_atlas_with_wgsl(
        renderer,
        &destination_texture,
        package.storage,
        output.format,
        output.width,
        output.height,
        overlay_generation,
    )?;
    renderer.retained_storage = package.storage;
    renderer.retained_cv_y_textures = std::array::from_fn(|_| None);
    renderer.retained_cv_uv_textures = std::array::from_fn(|_| None);
    Ok(submission)
}

fn render_cv_pixel_buffer_frame_set_to_metal_destination(
    renderer: &mut WgpuMetalRenderer,
    destination_mtl_texture: *mut core::ffi::c_void,
    source_y_mtl_textures: &[*mut core::ffi::c_void; MAX_TRACKS],
    source_uv_mtl_textures: &[*mut core::ffi::c_void; MAX_TRACKS],
    frame_set: &CVPixelBufferPresentFrameSet,
    output: OutputTarget,
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if frame_set.decision.should_present == 0 || frame_set.decision.frame_count <= 0 {
        return Err("wgpu-metal CVPixelBuffer frame set has no presentable frame");
    }
    let destination_texture = import_metal_texture_2d(
        &renderer.device,
        destination_mtl_texture,
        output.format,
        output.width,
        output.height,
        wgpu::TextureUsages::RENDER_ATTACHMENT,
        "voidplayer-imported-cvpixelbuffer-destination",
    )?;
    let mut source_y_textures: [Option<wgpu::Texture>; MAX_TRACKS] = std::array::from_fn(|_| None);
    let mut source_uv_textures: [Option<wgpu::Texture>; MAX_TRACKS] = std::array::from_fn(|_| None);
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
        source_y_textures[slot] = Some(import_metal_texture_2d(
            &renderer.device,
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
        )?);
        source_uv_textures[slot] = Some(import_metal_texture_2d(
            &renderer.device,
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
        )?);
    }

    let package = PresentFramePackageInfo {
        storage: STORAGE_CV_PIXEL_BUFFER,
        width: output.width as i32,
        height: output.height as i32,
        max_track_slots: MAX_TRACKS as i32,
        stride_bytes: 0,
        track_stride_bytes: 0,
        used_bytes: 4,
        decision: frame_set.decision,
    };
    renderer.params_scratch.clear();
    package_params(
        &package,
        STORAGE_CV_PIXEL_BUFFER,
        output.color_mode,
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        0.0,
        &mut renderer.params_scratch,
    );
    renderer.package_bytes_scratch.clear();
    renderer.package_bytes_scratch.resize(4, 0);
    combined_overlay_rects(
        overlay_fill_rects,
        overlay_line_rects,
        overlay_motion_lines,
        &mut renderer.overlay_rects_scratch,
    );
    let submission = render_cv_pixel_buffer_frame_set_with_wgsl(
        renderer,
        &destination_texture,
        &source_y_textures,
        &source_uv_textures,
        output.format,
        output.width,
        output.height,
        overlay_generation,
    )?;
    renderer.retained_storage = STORAGE_CV_PIXEL_BUFFER;
    renderer.retained_cv_y_textures = source_y_textures;
    renderer.retained_cv_uv_textures = source_uv_textures;
    Ok(submission)
}

fn render_retained_source_to_metal_destination(
    renderer: &mut WgpuMetalRenderer,
    destination_mtl_texture: *mut core::ffi::c_void,
    decision: &PresentDecisionInfo,
    output: OutputTarget,
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    if renderer.retained_storage == STORAGE_NONE {
        return Err("wgpu-metal retained source cache is empty");
    }
    let destination_texture = import_metal_texture_2d(
        &renderer.device,
        destination_mtl_texture,
        output.format,
        output.width,
        output.height,
        wgpu::TextureUsages::RENDER_ATTACHMENT,
        "voidplayer-imported-retained-composite-destination",
    )?;
    let package = PresentFramePackageInfo {
        storage: renderer.retained_storage,
        width: output.width as i32,
        height: output.height as i32,
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
    match renderer.retained_storage {
        STORAGE_BGRA => {
            if renderer.source_texture.is_none() {
                return Err("wgpu-metal retained BGRA source cache is incomplete");
            }
            render_bgra_atlas_with_wgsl(
                renderer,
                &destination_texture,
                STORAGE_BGRA,
                output.format,
                output.width,
                output.height,
                overlay_generation,
            )
        }
        STORAGE_YUV => {
            if renderer.package_buffer.is_none() {
                return Err("wgpu-metal retained YUV package source cache is incomplete");
            }
            render_bgra_atlas_with_wgsl(
                renderer,
                &destination_texture,
                STORAGE_YUV,
                output.format,
                output.width,
                output.height,
                overlay_generation,
            )
        }
        STORAGE_CV_PIXEL_BUFFER => {
            let has_source = renderer
                .retained_cv_y_textures
                .iter()
                .zip(renderer.retained_cv_uv_textures.iter())
                .any(|(y, uv)| y.is_some() && uv.is_some());
            if !has_source {
                return Err("wgpu-metal retained CVPixelBuffer source cache is incomplete");
            }
            if renderer.package_bytes_scratch.is_empty() {
                renderer.package_bytes_scratch.resize(4, 0);
            }
            let y_textures = core::mem::replace(
                &mut renderer.retained_cv_y_textures,
                std::array::from_fn(|_| None),
            );
            let uv_textures = core::mem::replace(
                &mut renderer.retained_cv_uv_textures,
                std::array::from_fn(|_| None),
            );
            let result = render_cv_pixel_buffer_frame_set_with_wgsl(
                renderer,
                &destination_texture,
                &y_textures,
                &uv_textures,
                output.format,
                output.width,
                output.height,
                overlay_generation,
            );
            renderer.retained_cv_y_textures = y_textures;
            renderer.retained_cv_uv_textures = uv_textures;
            result
        }
        _ => Err("wgpu-metal retained source storage is unsupported"),
    }
}

fn bgra_atlas_for_wgsl(
    source: &[u8],
    package: &PresentFramePackageInfo,
    atlas: &mut Vec<u8>,
) -> Result<(), &'static str> {
    if package.stride_bytes < package.width * 4 || package.track_stride_bytes == 0 {
        return Err("wgpu-metal BGRA package layout is invalid");
    }
    if package.used_bytes > source.len() {
        return Err("wgpu-metal BGRA package data is undersized");
    }
    let row_bytes = package.width as usize * 4;
    let track_bytes = row_bytes
        .checked_mul(package.height as usize)
        .ok_or("wgpu-metal BGRA atlas track overflow")?;
    let atlas_len = track_bytes
        .checked_mul(MAX_TRACKS)
        .ok_or("wgpu-metal BGRA atlas overflow")?;
    atlas.clear();
    atlas.resize(atlas_len, 0);
    for slot in 0..MAX_TRACKS {
        let src_track = package
            .track_stride_bytes
            .checked_mul(slot)
            .ok_or("wgpu-metal BGRA track offset overflow")?;
        if src_track >= package.used_bytes {
            continue;
        }
        let dst_track = track_bytes
            .checked_mul(slot)
            .ok_or("wgpu-metal BGRA atlas offset overflow")?;
        for y in 0..package.height as usize {
            let src_row = src_track
                .checked_add(
                    y.checked_mul(package.stride_bytes as usize)
                        .ok_or("wgpu-metal BGRA row offset overflow")?,
                )
                .ok_or("wgpu-metal BGRA row offset overflow")?;
            let dst_row = dst_track
                .checked_add(
                    y.checked_mul(row_bytes)
                        .ok_or("wgpu-metal BGRA atlas row overflow")?,
                )
                .ok_or("wgpu-metal BGRA atlas row overflow")?;
            if src_row + row_bytes > package.used_bytes || src_row + row_bytes > source.len() {
                return Err("wgpu-metal BGRA source row is out of bounds");
            }
            atlas[dst_row..dst_row + row_bytes]
                .copy_from_slice(&source[src_row..src_row + row_bytes]);
        }
    }
    Ok(())
}

fn package_storage_bytes(source: &[u8], bytes: &mut Vec<u8>) {
    let padded_len = source.len().max(4).next_multiple_of(4);
    bytes.clear();
    bytes.resize(padded_len, 0);
    bytes[..source.len()].copy_from_slice(source);
}

fn dummy_package_storage_bytes(bytes: &mut Vec<u8>) {
    bytes.clear();
    bytes.extend_from_slice(&[0, 0, 0, 0]);
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
    overlay_fill_rects: &[OverlayRect],
    overlay_line_rects: &[OverlayRect],
    overlay_motion_lines: &[OverlayRect],
    overlay_layer_track: f32,
    bytes: &mut Vec<u8>,
) {
    let decision = &package.decision;
    bytes.clear();
    bytes.reserve(24 * 16);
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
    push_vec4_i32(bytes, [output_color_mode, 0, 0, 0]);
}

fn write_source_bgra_atlas(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    cache: &mut Option<CachedSourceTexture>,
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
        texture.width != width || texture.height != height || texture.layers != layers
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
        *cache = Some(CachedSourceTexture {
            width,
            height,
            layers,
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

fn grow_buffer_capacity(size: u64) -> u64 {
    let min_size = size.max(4);
    min_size.checked_next_power_of_two().unwrap_or(min_size)
}

fn write_cached_storage_buffer(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    cache: &mut Option<CachedStorageBuffer>,
    bytes: &[u8],
    label: &'static str,
) {
    let size = bytes.len().max(4) as u64;
    let recreate = cache.as_ref().map_or(true, |buffer| buffer.capacity < size);
    if recreate {
        let capacity = grow_buffer_capacity(size);
        *cache = Some(CachedStorageBuffer {
            capacity,
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
                && cache.generation == overlay_generation
                && cache.fill_count == fill_count
                && cache.line_count == line_count
                && cache.motion_count == motion_count
        });
    if cache_hit
        && (overlay_generation != 0 || (fill_count == 0 && line_count == 0 && motion_count == 0))
    {
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
            usage: wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        renderer.overlay_layer_texture = Some(CachedOverlayLayerTexture {
            width,
            height,
            layers,
            generation: overlay_generation,
            fill_count,
            line_count,
            motion_count,
            texture,
            view,
        });
    }
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.overlay_buffer,
        overlay_rect_bytes(&renderer.overlay_rects_scratch),
        "voidplayer-wgpu-overlay-combined-rects",
    );
    let source_view = &renderer.dummy_bgra_array_view;
    let dummy_y_view = &renderer.dummy_y_view;
    let dummy_uv_view = &renderer.dummy_uv_view;
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
    let overlay_texture = &renderer
        .overlay_layer_texture
        .as_ref()
        .ok_or("wgpu-metal overlay layer texture is unavailable")?
        .texture;
    let final_params = renderer.params_scratch.clone();
    let mut layer_params = Vec::with_capacity(final_params.len());
    for track in 0..MAX_TRACKS {
        write_overlay_layer_track_params(&final_params, track, &mut layer_params);
        write_cached_storage_buffer(
            &renderer.device,
            &renderer.queue,
            &mut renderer.overlay_layer_params_buffers[track],
            &layer_params,
            "voidplayer-wgpu-overlay-layer-params",
        );
        let params_buffer = &renderer
            .overlay_layer_params_buffers[track]
            .as_ref()
            .ok_or("wgpu-metal overlay layer params buffer cache is unavailable")?
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
                ],
            });
        let layer_view = overlay_texture.create_view(&wgpu::TextureViewDescriptor {
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
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("voidplayer-wgpu-overlay-layer-pass"),
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
            pass.set_pipeline(&renderer.overlay_pipeline);
            pass.set_bind_group(0, &bind_group, &[]);
            pass.set_viewport(0.0, 0.0, width as f32, height as f32, 0.0, 1.0);
            pass.draw(0..3, 0..1);
        }
    }
    if let Some(cache) = renderer.overlay_layer_texture.as_mut() {
        cache.generation = overlay_generation;
        cache.fill_count = fill_count;
        cache.line_count = line_count;
        cache.motion_count = motion_count;
    }
    Ok(())
}

fn render_bgra_atlas_with_wgsl(
    renderer: &mut WgpuMetalRenderer,
    destination_texture: &wgpu::Texture,
    source_storage: i32,
    output_format: wgpu::TextureFormat,
    width: u32,
    height: u32,
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.package_buffer,
        &renderer.package_bytes_scratch,
        "voidplayer-wgpu-package-bytes",
    );
    let mut encoder = renderer
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("voidplayer-wgpu-composite-encoder"),
        });
    encode_overlay_layer_texture(renderer, width, height, overlay_generation, &mut encoder)?;
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.params_buffer,
        &renderer.params_scratch,
        "voidplayer-wgpu-composite-params",
    );

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
    let source_view: &wgpu::TextureView = if source_storage == STORAGE_BGRA {
        &renderer
            .source_texture
            .as_ref()
            .ok_or("wgpu-metal source texture cache is unavailable")?
            .view
    } else {
        &renderer.dummy_bgra_array_view
    };
    let dummy_y_view = &renderer.dummy_y_view;
    let dummy_uv_view = &renderer.dummy_uv_view;
    let destination_view = destination_texture.create_view(&wgpu::TextureViewDescriptor::default());
    let pipeline = composite_pipeline_for_output(renderer, output_format)?;
    let bind_group = renderer
        .device
        .create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("voidplayer-wgpu-composite-bind-group"),
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
                    resource: wgpu::BindingResource::TextureView(overlay_layer_texture),
                },
            ],
        });
    {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("voidplayer-wgpu-composite-pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &destination_view,
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
    let submission = renderer.queue.submit(std::iter::once(encoder.finish()));
    Ok(submission)
}

fn render_cv_pixel_buffer_frame_set_with_wgsl(
    renderer: &mut WgpuMetalRenderer,
    destination_texture: &wgpu::Texture,
    source_y_textures: &[Option<wgpu::Texture>; MAX_TRACKS],
    source_uv_textures: &[Option<wgpu::Texture>; MAX_TRACKS],
    output_format: wgpu::TextureFormat,
    width: u32,
    height: u32,
    overlay_generation: u64,
) -> Result<wgpu::SubmissionIndex, &'static str> {
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.package_buffer,
        &renderer.package_bytes_scratch,
        "voidplayer-wgpu-package-bytes",
    );
    let mut encoder = renderer
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("voidplayer-wgpu-cvpixelbuffer-composite-encoder"),
        });
    encode_overlay_layer_texture(renderer, width, height, overlay_generation, &mut encoder)?;
    write_cached_storage_buffer(
        &renderer.device,
        &renderer.queue,
        &mut renderer.params_buffer,
        &renderer.params_scratch,
        "voidplayer-wgpu-composite-params",
    );

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
    let source_view = &renderer.dummy_bgra_array_view;
    let y_views: [Option<wgpu::TextureView>; MAX_TRACKS] = std::array::from_fn(|slot| {
        source_y_textures[slot]
            .as_ref()
            .map(|texture| texture.create_view(&wgpu::TextureViewDescriptor::default()))
    });
    let uv_views: [Option<wgpu::TextureView>; MAX_TRACKS] = std::array::from_fn(|slot| {
        source_uv_textures[slot]
            .as_ref()
            .map(|texture| texture.create_view(&wgpu::TextureViewDescriptor::default()))
    });
    let y_view = |slot: usize| y_views[slot].as_ref().unwrap_or(&renderer.dummy_y_view);
    let uv_view = |slot: usize| uv_views[slot].as_ref().unwrap_or(&renderer.dummy_uv_view);
    let destination_view = destination_texture.create_view(&wgpu::TextureViewDescriptor::default());
    let pipeline = composite_pipeline_for_output(renderer, output_format)?;
    let bind_group = renderer
        .device
        .create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("voidplayer-wgpu-cvpixelbuffer-composite-bind-group"),
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
            ],
        });
    {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("voidplayer-wgpu-cvpixelbuffer-composite-pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &destination_view,
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
    let submission = renderer.queue.submit(std::iter::once(encoder.finish()));
    Ok(submission)
}
