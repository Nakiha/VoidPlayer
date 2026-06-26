use std::time::Instant;
use windows::core::Interface;

use crate::{MAX_TRACKS, YUV_FORMAT_NV12};

pub const D3D12_TEXTURE_FORMAT_NV12: i32 = 1;
pub const D3D12_TEXTURE_FORMAT_P010: i32 = 2;
pub const D3D12_TEXTURE_FORMAT_BGRA8_UNORM: i32 = 3;
pub const D3D12_TEXTURE_FORMAT_RGBA16_FLOAT: i32 = 4;
pub const OUTPUT_COLOR_MODE_SDR: i32 = 1;
pub const OUTPUT_COLOR_MODE_EDR: i32 = 2;
const STORAGE_CV_PIXEL_BUFFER: i32 = 3;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct D3D12PresentFrameInfo {
    pub present: i32,
    pub file_id: i32,
    pub slot: i32,
    pub width: i32,
    pub height: i32,
    pub pts_us: i64,
    pub dts_us: i64,
    pub duration_us: i64,
    pub analysis_frame_index: i32,
    pub frame_identity_mode: i32,
    pub source_packet_index: i32,
    pub source_packet_size: i32,
    pub source_packet_pos: i64,
    pub source_packet_pts: i64,
    pub source_packet_dts: i64,
    pub color_range: i32,
    pub color_matrix: i32,
    pub color_transfer: i32,
    pub color_primaries: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3D12PresentDecisionInfo {
    pub should_present: i32,
    pub frame_count: i32,
    pub track_count: i32,
    pub mode: i32,
    pub current_pts_us: i64,
    pub split_pos: f32,
    pub background_color: [f32; 4],
    pub order: [i32; MAX_TRACKS],
    pub display_offset_x: [f32; MAX_TRACKS],
    pub display_offset_y: [f32; MAX_TRACKS],
    pub inv_display_size_x: [f32; MAX_TRACKS],
    pub inv_display_size_y: [f32; MAX_TRACKS],
    pub view_offset_uv_x: [f32; MAX_TRACKS],
    pub view_offset_uv_y: [f32; MAX_TRACKS],
    pub source_width: [i32; MAX_TRACKS],
    pub source_height: [i32; MAX_TRACKS],
    pub yuv_format: [i32; MAX_TRACKS],
    pub y_offset: [i32; MAX_TRACKS],
    pub uv_offset: [i32; MAX_TRACKS],
    pub v_offset: [i32; MAX_TRACKS],
    pub y_stride: [i32; MAX_TRACKS],
    pub uv_stride: [i32; MAX_TRACKS],
    pub coded_width: [i32; MAX_TRACKS],
    pub coded_height: [i32; MAX_TRACKS],
    pub nv12_uv_scale_x: [f32; MAX_TRACKS],
    pub nv12_uv_scale_y: [f32; MAX_TRACKS],
    pub color_range: [i32; MAX_TRACKS],
    pub color_matrix: [i32; MAX_TRACKS],
    pub color_transfer: [i32; MAX_TRACKS],
    pub color_primaries: [i32; MAX_TRACKS],
    pub frames: [D3D12PresentFrameInfo; MAX_TRACKS],
}

impl Default for D3D12PresentDecisionInfo {
    fn default() -> Self {
        Self {
            should_present: 0,
            frame_count: 0,
            track_count: 0,
            mode: 0,
            current_pts_us: 0,
            split_pos: 0.5,
            background_color: [0.0; 4],
            order: [0, 1, 2, 3],
            display_offset_x: [0.0; MAX_TRACKS],
            display_offset_y: [0.0; MAX_TRACKS],
            inv_display_size_x: [1.0; MAX_TRACKS],
            inv_display_size_y: [1.0; MAX_TRACKS],
            view_offset_uv_x: [0.0; MAX_TRACKS],
            view_offset_uv_y: [0.0; MAX_TRACKS],
            source_width: [1; MAX_TRACKS],
            source_height: [1; MAX_TRACKS],
            yuv_format: [YUV_FORMAT_NV12; MAX_TRACKS],
            y_offset: [0; MAX_TRACKS],
            uv_offset: [0; MAX_TRACKS],
            v_offset: [0; MAX_TRACKS],
            y_stride: [0; MAX_TRACKS],
            uv_stride: [0; MAX_TRACKS],
            coded_width: [1; MAX_TRACKS],
            coded_height: [1; MAX_TRACKS],
            nv12_uv_scale_x: [1.0; MAX_TRACKS],
            nv12_uv_scale_y: [1.0; MAX_TRACKS],
            color_range: [1; MAX_TRACKS],
            color_matrix: [2; MAX_TRACKS],
            color_transfer: [1; MAX_TRACKS],
            color_primaries: [2; MAX_TRACKS],
            frames: [D3D12PresentFrameInfo::default(); MAX_TRACKS],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgpuD3D12CompositeRequest {
    pub destination_resource: *mut core::ffi::c_void,
    pub output_format: i32,
    pub output_color_mode: i32,
    pub flutter_resource: *mut core::ffi::c_void,
    pub flutter_format: i32,
    pub flutter_width: u32,
    pub flutter_height: u32,
    pub source_resources: [*mut core::ffi::c_void; MAX_TRACKS],
    pub source_formats: [i32; MAX_TRACKS],
    pub source_array_layers: [u32; MAX_TRACKS],
    pub source_base_array_layers: [u32; MAX_TRACKS],
    pub cpu_sources: [WgpuD3D12CpuSourceInfo; MAX_TRACKS],
    pub decision: *const D3D12PresentDecisionInfo,
    pub width: i32,
    pub height: i32,
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgpuD3D12CpuSourceInfo {
    pub y_data: *const core::ffi::c_void,
    pub y_size: usize,
    pub uv_data: *const core::ffi::c_void,
    pub uv_size: usize,
    pub format: i32,
    pub y_stride: i32,
    pub uv_stride: i32,
    pub y_width: u32,
    pub y_height: u32,
    pub uv_width: u32,
    pub uv_height: u32,
}

impl Default for WgpuD3D12CpuSourceInfo {
    fn default() -> Self {
        Self {
            y_data: core::ptr::null(),
            y_size: 0,
            uv_data: core::ptr::null(),
            uv_size: 0,
            format: D3D12_TEXTURE_FORMAT_NV12,
            y_stride: 0,
            uv_stride: 0,
            y_width: 0,
            y_height: 0,
            uv_width: 0,
            uv_height: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WgpuD3D12ProfilerSnapshot {
    pub destination_import_count: u64,
    pub source_import_count: u64,
    pub submit_count: u64,
    pub last_import_us: u64,
    pub last_prepare_us: u64,
    pub last_pass_encode_us: u64,
    pub last_submit_us: u64,
    pub last_cpu_render_us: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgpuD3D12TextureImportRequest {
    pub d3d12_resource: *mut core::ffi::c_void,
    pub format: i32,
    pub width: u32,
    pub height: u32,
    pub array_layers: u32,
    pub mip_levels: u32,
    pub sample_count: u32,
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WgpuD3D12RenderTargetClearRequest {
    pub d3d12_resource: *mut core::ffi::c_void,
    pub format: i32,
    pub width: u32,
    pub height: u32,
    pub color: [f32; 4],
    pub error: *mut core::ffi::c_char,
    pub error_size: usize,
}

pub struct WgpuD3D12Renderer {
    adapter_info: wgpu::AdapterInfo,
    device: wgpu::Device,
    queue: wgpu::Queue,
    sampler: wgpu::Sampler,
    bind_group_layout: wgpu::BindGroupLayout,
    bgra8_pipeline: wgpu::RenderPipeline,
    rgba16_float_pipeline: wgpu::RenderPipeline,
    _dummy_bgra_array_texture: wgpu::Texture,
    dummy_bgra_array_view: wgpu::TextureView,
    _dummy_y_texture: wgpu::Texture,
    dummy_y_view: wgpu::TextureView,
    _dummy_uv_texture: wgpu::Texture,
    dummy_uv_view: wgpu::TextureView,
    _dummy_overlay_texture: wgpu::Texture,
    dummy_overlay_view: wgpu::TextureView,
    _dummy_flutter_texture: wgpu::Texture,
    dummy_flutter_view: wgpu::TextureView,
    params_buffer: Option<wgpu::Buffer>,
    package_buffer: wgpu::Buffer,
    overlay_buffer: wgpu::Buffer,
    profiler: WgpuD3D12ProfilerSnapshot,
    supports_nv12: bool,
    supports_p010: bool,
    supports_rgba16_float: bool,
}

impl WgpuD3D12Renderer {
    pub fn new() -> Result<Self, &'static str> {
        let mut instance_desc = wgpu::InstanceDescriptor::new_without_display_handle();
        instance_desc.backends = wgpu::Backends::DX12;
        let instance = wgpu::Instance::new(instance_desc);
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .map_err(|_| "wgpu-d3d12 adapter not found")?;

        let adapter_features = adapter.features();
        let mut required_features = wgpu::Features::empty();
        for feature in [
            wgpu::Features::TEXTURE_FORMAT_NV12,
            wgpu::Features::TEXTURE_FORMAT_P010,
            wgpu::Features::TEXTURE_FORMAT_16BIT_NORM,
        ] {
            if adapter_features.contains(feature) {
                required_features |= feature;
            }
        }

        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 device"),
            required_features,
            required_limits: wgpu::Limits::default(),
            experimental_features: wgpu::ExperimentalFeatures::disabled(),
            memory_hints: wgpu::MemoryHints::Performance,
            trace: wgpu::Trace::Off,
        }))
        .map_err(|_| "wgpu-d3d12 device creation failed")?;

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 composite sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 composite WGSL"),
            source: wgpu::ShaderSource::Wgsl(WGSL_COMPOSITE_SHADER.into()),
        });
        let bind_group_layout = create_composite_bind_group_layout(&device);
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 composite pipeline layout"),
            bind_group_layouts: &[Some(&bind_group_layout)],
            immediate_size: 0,
        });
        let bgra8_pipeline = create_composite_pipeline(
            &device,
            &pipeline_layout,
            &shader,
            wgpu::TextureFormat::Bgra8Unorm,
            "VoidPlayer wgpu-d3d12 BGRA8 composite pipeline",
        );
        let rgba16_float_pipeline = create_composite_pipeline(
            &device,
            &pipeline_layout,
            &shader,
            wgpu::TextureFormat::Rgba16Float,
            "VoidPlayer wgpu-d3d12 RGBA16F composite pipeline",
        );
        let dummy_bgra_array_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 dummy BGRA array"),
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
            label: Some("VoidPlayer wgpu-d3d12 dummy Y plane"),
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
            label: Some("VoidPlayer wgpu-d3d12 dummy UV plane"),
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
        let dummy_overlay_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 dummy overlay array"),
            size: wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: MAX_TRACKS as u32,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let dummy_overlay_view =
            dummy_overlay_texture.create_view(&wgpu::TextureViewDescriptor::default());
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &dummy_overlay_texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &[0u8; MAX_TRACKS * 4],
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(4),
                rows_per_image: Some(1),
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: MAX_TRACKS as u32,
            },
        );
        let dummy_flutter_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 dummy Flutter surface"),
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
            &[0u8; 4],
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
        let package_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 dummy package buffer"),
            size: 4,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(&package_buffer, 0, &[0u8; 4]);
        let overlay_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 dummy overlay buffer"),
            size: 16,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(&overlay_buffer, 0, &[0u8; 16]);
        let adapter_info = adapter.get_info();
        Ok(Self {
            adapter_info,
            device,
            queue,
            sampler,
            bind_group_layout,
            bgra8_pipeline,
            rgba16_float_pipeline,
            _dummy_bgra_array_texture: dummy_bgra_array_texture,
            dummy_bgra_array_view,
            _dummy_y_texture: dummy_y_texture,
            dummy_y_view,
            _dummy_uv_texture: dummy_uv_texture,
            dummy_uv_view,
            _dummy_overlay_texture: dummy_overlay_texture,
            dummy_overlay_view,
            _dummy_flutter_texture: dummy_flutter_texture,
            dummy_flutter_view,
            params_buffer: None,
            package_buffer,
            overlay_buffer,
            profiler: WgpuD3D12ProfilerSnapshot::default(),
            supports_nv12: required_features.contains(wgpu::Features::TEXTURE_FORMAT_NV12),
            supports_p010: required_features.contains(wgpu::Features::TEXTURE_FORMAT_P010),
            supports_rgba16_float: texture_format_supported(
                &adapter,
                wgpu::TextureFormat::Rgba16Float,
            ),
        })
    }

    pub fn adapter_info(&self) -> &wgpu::AdapterInfo {
        &self.adapter_info
    }

    pub fn profiler_snapshot(&self) -> WgpuD3D12ProfilerSnapshot {
        self.profiler
    }

    pub fn supports_nv12(&self) -> bool {
        self.supports_nv12
    }

    pub fn supports_p010(&self) -> bool {
        self.supports_p010
    }

    pub fn supports_rgba16_float(&self) -> bool {
        self.supports_rgba16_float
    }

    pub fn d3d12_device_ptr(&self) -> *mut core::ffi::c_void {
        unsafe {
            self.device
                .as_hal::<wgpu_hal::api::Dx12>()
                .map(|device| device.raw_device().as_raw() as *mut core::ffi::c_void)
                .unwrap_or(core::ptr::null_mut())
        }
    }

    pub fn import_texture_for_probe(
        &mut self,
        request: &WgpuD3D12TextureImportRequest,
    ) -> Result<(), &'static str> {
        let start = Instant::now();
        let format = d3d12_texture_format(request.format)?;
        if format == wgpu::TextureFormat::NV12 && !self.supports_nv12 {
            return Err("wgpu-d3d12 adapter does not support NV12 texture import");
        }
        if format == wgpu::TextureFormat::P010 && !self.supports_p010 {
            return Err("wgpu-d3d12 adapter does not support P010 texture import");
        }
        if request.d3d12_resource.is_null() {
            return Err("wgpu-d3d12 texture import requires a D3D12 resource");
        }
        if request.width == 0 || request.height == 0 {
            return Err("wgpu-d3d12 texture import requires non-zero dimensions");
        }

        let mip_levels = request.mip_levels.max(1);
        let sample_count = request.sample_count.max(1);
        let depth_or_array_layers = request.array_layers.max(1);
        let texture = unsafe {
            import_d3d12_resource(
                &self.device,
                request.d3d12_resource,
                format,
                wgpu::TextureUsages::TEXTURE_BINDING,
                "VoidPlayer imported D3D12VA frame",
                wgpu::Extent3d {
                    width: request.width,
                    height: request.height,
                    depth_or_array_layers,
                },
                mip_levels,
                sample_count,
            )
        }?;
        drop(texture);
        self.profiler.source_import_count = self.profiler.source_import_count.saturating_add(1);
        self.profiler.last_import_us = elapsed_us(start);
        Ok(())
    }

    pub fn clear_render_target_for_probe(
        &mut self,
        request: &WgpuD3D12RenderTargetClearRequest,
    ) -> Result<(), &'static str> {
        let start = Instant::now();
        let format = d3d12_texture_format(request.format)?;
        if format != wgpu::TextureFormat::Rgba16Float && format != wgpu::TextureFormat::Bgra8Unorm {
            return Err("wgpu-d3d12 render target clear requires BGRA8 or RGBA16F");
        }
        if format == wgpu::TextureFormat::Rgba16Float && !self.supports_rgba16_float {
            return Err("wgpu-d3d12 adapter does not support RGBA16F render targets");
        }
        if request.d3d12_resource.is_null() {
            return Err("wgpu-d3d12 render target clear requires a D3D12 resource");
        }
        if request.width == 0 || request.height == 0 {
            return Err("wgpu-d3d12 render target clear requires non-zero dimensions");
        }

        let import_start = Instant::now();
        let texture = unsafe {
            import_d3d12_resource(
                &self.device,
                request.d3d12_resource,
                format,
                wgpu::TextureUsages::RENDER_ATTACHMENT,
                "VoidPlayer imported D3D12 render target",
                wgpu::Extent3d {
                    width: request.width,
                    height: request.height,
                    depth_or_array_layers: 1,
                },
                1,
                1,
            )
        }?;
        self.profiler.destination_import_count =
            self.profiler.destination_import_count.saturating_add(1);
        self.profiler.last_import_us = elapsed_us(import_start);

        let prepare_start = Instant::now();
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        self.profiler.last_prepare_us = elapsed_us(prepare_start);

        let encode_start = Instant::now();
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("VoidPlayer wgpu-d3d12 clear encoder"),
            });
        {
            let _pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("VoidPlayer wgpu-d3d12 clear pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color {
                            r: request.color[0] as f64,
                            g: request.color[1] as f64,
                            b: request.color[2] as f64,
                            a: request.color[3] as f64,
                        }),
                        store: wgpu::StoreOp::Store,
                    },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
        }
        self.profiler.last_pass_encode_us = elapsed_us(encode_start);

        let submit_start = Instant::now();
        let submission = self.queue.submit(Some(encoder.finish()));
        self.device
            .poll(wgpu::PollType::Wait {
                submission_index: Some(submission),
                timeout: None,
            })
            .map_err(|_| "wgpu-d3d12 render target clear wait failed")?;
        self.profiler.submit_count = self.profiler.submit_count.saturating_add(1);
        self.profiler.last_submit_us = elapsed_us(submit_start);
        self.profiler.last_cpu_render_us = elapsed_us(start);
        Ok(())
    }

    pub fn render_composite(
        &mut self,
        request: &WgpuD3D12CompositeRequest,
    ) -> Result<(), &'static str> {
        let start = Instant::now();
        if request.destination_resource.is_null() {
            return Err("wgpu-d3d12 composite destination is null");
        }
        if request.decision.is_null() {
            return Err("wgpu-d3d12 composite decision is null");
        }
        if request.width <= 0 || request.height <= 0 {
            return Err("wgpu-d3d12 composite target dimensions are invalid");
        }
        let output_format = d3d12_texture_format(request.output_format)?;
        if output_format != wgpu::TextureFormat::Rgba16Float
            && output_format != wgpu::TextureFormat::Bgra8Unorm
        {
            return Err("wgpu-d3d12 composite destination format is unsupported");
        }
        let decision = unsafe { &*request.decision };
        if decision.should_present == 0 || decision.frame_count <= 0 {
            return Err("wgpu-d3d12 composite has no presentable frame");
        }

        let import_start = Instant::now();
        let destination = unsafe {
            import_d3d12_resource(
                &self.device,
                request.destination_resource,
                output_format,
                wgpu::TextureUsages::RENDER_ATTACHMENT,
                "VoidPlayer imported D3D12 composite destination",
                wgpu::Extent3d {
                    width: request.width as u32,
                    height: request.height as u32,
                    depth_or_array_layers: 1,
                },
                1,
                1,
            )
        }?;
        let destination_view = destination.create_view(&wgpu::TextureViewDescriptor::default());
        self.profiler.destination_import_count =
            self.profiler.destination_import_count.saturating_add(1);

        let mut source_textures: [Option<wgpu::Texture>; MAX_TRACKS] =
            std::array::from_fn(|_| None);
        let mut source_y_textures: [Option<wgpu::Texture>; MAX_TRACKS] =
            std::array::from_fn(|_| None);
        let mut source_uv_textures: [Option<wgpu::Texture>; MAX_TRACKS] =
            std::array::from_fn(|_| None);
        let mut source_y_views: [Option<wgpu::TextureView>; MAX_TRACKS] =
            std::array::from_fn(|_| None);
        let mut source_uv_views: [Option<wgpu::TextureView>; MAX_TRACKS] =
            std::array::from_fn(|_| None);
        let mut flutter_texture: Option<wgpu::Texture> = None;
        let mut flutter_view: Option<wgpu::TextureView> = None;
        if !request.flutter_resource.is_null() {
            let flutter_format = d3d12_texture_format(request.flutter_format)?;
            if flutter_format != wgpu::TextureFormat::Bgra8Unorm {
                return Err("wgpu-d3d12 Flutter composite source must be BGRA8");
            }
            if request.flutter_width == 0 || request.flutter_height == 0 {
                return Err("wgpu-d3d12 Flutter composite dimensions are invalid");
            }
            let texture = unsafe {
                import_d3d12_resource(
                    &self.device,
                    request.flutter_resource,
                    flutter_format,
                    wgpu::TextureUsages::TEXTURE_BINDING,
                    "VoidPlayer imported Flutter D3D12 surface",
                    wgpu::Extent3d {
                        width: request.flutter_width,
                        height: request.flutter_height,
                        depth_or_array_layers: 1,
                    },
                    1,
                    1,
                )
            }?;
            flutter_view = Some(texture.create_view(&wgpu::TextureViewDescriptor {
                label: Some("VoidPlayer imported Flutter D3D12 surface view"),
                format: Some(wgpu::TextureFormat::Bgra8Unorm),
                dimension: Some(wgpu::TextureViewDimension::D2),
                usage: Some(wgpu::TextureUsages::TEXTURE_BINDING),
                aspect: wgpu::TextureAspect::All,
                base_mip_level: 0,
                mip_level_count: Some(1),
                base_array_layer: 0,
                array_layer_count: Some(1),
            }));
            flutter_texture = Some(texture);
            self.profiler.source_import_count = self.profiler.source_import_count.saturating_add(1);
        }
        for slot in 0..MAX_TRACKS {
            if decision.frames[slot].present == 0 {
                continue;
            }
            let format = d3d12_texture_format(request.source_formats[slot])?;
            if format == wgpu::TextureFormat::NV12 && !self.supports_nv12 {
                return Err("wgpu-d3d12 adapter does not support NV12 composite");
            }
            if format == wgpu::TextureFormat::P010 && !self.supports_p010 {
                return Err("wgpu-d3d12 adapter does not support P010 composite");
            }
            if request.source_resources[slot].is_null() {
                let cpu_source = request.cpu_sources[slot];
                let (y_texture, uv_texture, y_view, uv_view) =
                    self.upload_cpu_yuv_source(slot, format, &cpu_source)?;
                source_y_textures[slot] = Some(y_texture);
                source_uv_textures[slot] = Some(uv_texture);
                source_y_views[slot] = Some(y_view);
                source_uv_views[slot] = Some(uv_view);
                self.profiler.source_import_count =
                    self.profiler.source_import_count.saturating_add(1);
                continue;
            }
            let coded_width = decision.coded_width[slot]
                .max(decision.source_width[slot])
                .max(1);
            let coded_height = decision.coded_height[slot]
                .max(decision.source_height[slot])
                .max(1);
            let array_layers = request.source_array_layers[slot].max(1);
            let base_array_layer = request.source_base_array_layers[slot];
            if base_array_layer >= array_layers {
                return Err("wgpu-d3d12 composite source array layer is out of range");
            }
            let texture = unsafe {
                import_d3d12_resource(
                    &self.device,
                    request.source_resources[slot],
                    format,
                    wgpu::TextureUsages::TEXTURE_BINDING,
                    "VoidPlayer imported D3D12VA source",
                    wgpu::Extent3d {
                        width: coded_width as u32,
                        height: coded_height as u32,
                        depth_or_array_layers: array_layers,
                    },
                    1,
                    1,
                )
            }?;
            source_y_views[slot] = Some(texture.create_view(&wgpu::TextureViewDescriptor {
                label: Some("VoidPlayer imported D3D12VA source Y view"),
                format: Some(if format == wgpu::TextureFormat::P010 {
                    wgpu::TextureFormat::R16Unorm
                } else {
                    wgpu::TextureFormat::R8Unorm
                }),
                dimension: Some(wgpu::TextureViewDimension::D2),
                usage: Some(wgpu::TextureUsages::TEXTURE_BINDING),
                aspect: wgpu::TextureAspect::Plane0,
                base_mip_level: 0,
                mip_level_count: Some(1),
                base_array_layer,
                array_layer_count: Some(1),
            }));
            source_uv_views[slot] = Some(texture.create_view(&wgpu::TextureViewDescriptor {
                label: Some("VoidPlayer imported D3D12VA source UV view"),
                format: Some(if format == wgpu::TextureFormat::P010 {
                    wgpu::TextureFormat::Rg16Unorm
                } else {
                    wgpu::TextureFormat::Rg8Unorm
                }),
                dimension: Some(wgpu::TextureViewDimension::D2),
                usage: Some(wgpu::TextureUsages::TEXTURE_BINDING),
                aspect: wgpu::TextureAspect::Plane1,
                base_mip_level: 0,
                mip_level_count: Some(1),
                base_array_layer,
                array_layer_count: Some(1),
            }));
            source_textures[slot] = Some(texture);
            self.profiler.source_import_count = self.profiler.source_import_count.saturating_add(1);
        }
        self.profiler.last_import_us = elapsed_us(import_start);

        let prepare_start = Instant::now();
        let mut params = Vec::with_capacity(27 * 16);
        package_params(
            decision,
            request.width,
            request.height,
            STORAGE_CV_PIXEL_BUFFER,
            request.output_color_mode,
            &mut params,
        );
        write_storage_buffer(
            &self.device,
            &self.queue,
            &mut self.params_buffer,
            &params,
            "VoidPlayer wgpu-d3d12 composite params",
        );
        let params_buffer = self
            .params_buffer
            .as_ref()
            .ok_or("wgpu-d3d12 composite params buffer is unavailable")?;
        self.profiler.last_prepare_us = elapsed_us(prepare_start);

        let bind_group = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 composite bind group"),
            layout: &self.bind_group_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: params_buffer.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&self.dummy_bgra_array_view),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: self.package_buffer.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: self.overlay_buffer.as_entire_binding(),
                },
                cv_bind_entry(5, 0, &source_y_views, &self.dummy_y_view),
                cv_bind_entry(6, 0, &source_uv_views, &self.dummy_uv_view),
                cv_bind_entry(7, 1, &source_y_views, &self.dummy_y_view),
                cv_bind_entry(8, 1, &source_uv_views, &self.dummy_uv_view),
                cv_bind_entry(9, 2, &source_y_views, &self.dummy_y_view),
                cv_bind_entry(10, 2, &source_uv_views, &self.dummy_uv_view),
                cv_bind_entry(11, 3, &source_y_views, &self.dummy_y_view),
                cv_bind_entry(12, 3, &source_uv_views, &self.dummy_uv_view),
                wgpu::BindGroupEntry {
                    binding: 13,
                    resource: wgpu::BindingResource::TextureView(&self.dummy_overlay_view),
                },
                wgpu::BindGroupEntry {
                    binding: 14,
                    resource: wgpu::BindingResource::TextureView(
                        flutter_view.as_ref().unwrap_or(&self.dummy_flutter_view),
                    ),
                },
            ],
        });

        let encode_start = Instant::now();
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("VoidPlayer wgpu-d3d12 composite encoder"),
            });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("VoidPlayer wgpu-d3d12 composite pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &destination_view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            let pipeline = match output_format {
                wgpu::TextureFormat::Rgba16Float => &self.rgba16_float_pipeline,
                wgpu::TextureFormat::Bgra8Unorm => &self.bgra8_pipeline,
                _ => return Err("wgpu-d3d12 composite pipeline format is unsupported"),
            };
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &bind_group, &[]);
            pass.set_viewport(
                0.0,
                0.0,
                request.width as f32,
                request.height as f32,
                0.0,
                1.0,
            );
            pass.draw(0..3, 0..1);
        }
        self.profiler.last_pass_encode_us = elapsed_us(encode_start);

        let submit_start = Instant::now();
        let submission = self.queue.submit(Some(encoder.finish()));
        self.device
            .poll(wgpu::PollType::Wait {
                submission_index: Some(submission),
                timeout: None,
            })
            .map_err(|_| "wgpu-d3d12 composite wait failed")?;
        self.profiler.submit_count = self.profiler.submit_count.saturating_add(1);
        self.profiler.last_submit_us = elapsed_us(submit_start);
        self.profiler.last_cpu_render_us = elapsed_us(start);
        drop(source_textures);
        drop(flutter_texture);
        Ok(())
    }

    fn upload_cpu_yuv_source(
        &self,
        slot: usize,
        format: wgpu::TextureFormat,
        source: &WgpuD3D12CpuSourceInfo,
    ) -> Result<
        (
            wgpu::Texture,
            wgpu::Texture,
            wgpu::TextureView,
            wgpu::TextureView,
        ),
        &'static str,
    > {
        if source.y_data.is_null() || source.uv_data.is_null() {
            return Err("wgpu-d3d12 CPU composite source planes are null");
        }
        let high_bit = format == wgpu::TextureFormat::P010;
        if format != wgpu::TextureFormat::NV12 && format != wgpu::TextureFormat::P010 {
            return Err("wgpu-d3d12 CPU composite source format is unsupported");
        }
        let y_sample_bytes = if high_bit { 2usize } else { 1usize };
        let uv_pixel_bytes = if high_bit { 4usize } else { 2usize };
        let y_width = source.y_width.max(1);
        let y_height = source.y_height.max(1);
        let uv_width = source.uv_width.max(1);
        let uv_height = source.uv_height.max(1);
        let y_stride =
            usize::try_from(source.y_stride).map_err(|_| "wgpu-d3d12 CPU Y stride is invalid")?;
        let uv_stride =
            usize::try_from(source.uv_stride).map_err(|_| "wgpu-d3d12 CPU UV stride is invalid")?;
        let y_row_bytes = y_width as usize * y_sample_bytes;
        let uv_row_bytes = uv_width as usize * uv_pixel_bytes;
        if y_stride < y_row_bytes || uv_stride < uv_row_bytes {
            return Err("wgpu-d3d12 CPU source stride is smaller than row bytes");
        }
        let required_y = required_plane_bytes(y_stride, y_row_bytes, y_height as usize)?;
        let required_uv = required_plane_bytes(uv_stride, uv_row_bytes, uv_height as usize)?;
        if source.y_size < required_y || source.uv_size < required_uv {
            return Err("wgpu-d3d12 CPU source plane buffer is too small");
        }

        let y_format = if high_bit {
            wgpu::TextureFormat::R16Unorm
        } else {
            wgpu::TextureFormat::R8Unorm
        };
        let uv_format = if high_bit {
            wgpu::TextureFormat::Rg16Unorm
        } else {
            wgpu::TextureFormat::Rg8Unorm
        };
        let y_texture = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 CPU Y plane"),
            size: wgpu::Extent3d {
                width: y_width,
                height: y_height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: y_format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let uv_texture = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 CPU UV plane"),
            size: wgpu::Extent3d {
                width: uv_width,
                height: uv_height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: uv_format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });

        let y_bytes =
            unsafe { core::slice::from_raw_parts(source.y_data.cast::<u8>(), source.y_size) };
        let uv_bytes =
            unsafe { core::slice::from_raw_parts(source.uv_data.cast::<u8>(), source.uv_size) };
        self.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &y_texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            y_bytes,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(y_stride as u32),
                rows_per_image: Some(y_height),
            },
            wgpu::Extent3d {
                width: y_width,
                height: y_height,
                depth_or_array_layers: 1,
            },
        );
        self.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &uv_texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            uv_bytes,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(uv_stride as u32),
                rows_per_image: Some(uv_height),
            },
            wgpu::Extent3d {
                width: uv_width,
                height: uv_height,
                depth_or_array_layers: 1,
            },
        );

        let y_view = y_texture.create_view(&wgpu::TextureViewDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 CPU Y view"),
            format: Some(y_format),
            dimension: Some(wgpu::TextureViewDimension::D2),
            usage: Some(wgpu::TextureUsages::TEXTURE_BINDING),
            aspect: wgpu::TextureAspect::All,
            base_mip_level: 0,
            mip_level_count: Some(1),
            base_array_layer: 0,
            array_layer_count: Some(1),
        });
        let uv_view = uv_texture.create_view(&wgpu::TextureViewDescriptor {
            label: Some("VoidPlayer wgpu-d3d12 CPU UV view"),
            format: Some(uv_format),
            dimension: Some(wgpu::TextureViewDimension::D2),
            usage: Some(wgpu::TextureUsages::TEXTURE_BINDING),
            aspect: wgpu::TextureAspect::All,
            base_mip_level: 0,
            mip_level_count: Some(1),
            base_array_layer: 0,
            array_layer_count: Some(1),
        });
        let _ = slot;
        Ok((y_texture, uv_texture, y_view, uv_view))
    }
}

fn required_plane_bytes(
    stride: usize,
    row_bytes: usize,
    height: usize,
) -> Result<usize, &'static str> {
    if height == 0 {
        return Ok(0);
    }
    stride
        .checked_mul(height - 1)
        .and_then(|value| value.checked_add(row_bytes))
        .ok_or("wgpu-d3d12 CPU source plane size overflow")
}

pub static WGSL_COMPOSITE_SHADER: &str = include_str!("../shaders/composite.wgsl");

fn texture_format_supported(adapter: &wgpu::Adapter, format: wgpu::TextureFormat) -> bool {
    let features = adapter.get_texture_format_features(format);
    features
        .allowed_usages
        .contains(wgpu::TextureUsages::RENDER_ATTACHMENT)
        || features
            .allowed_usages
            .contains(wgpu::TextureUsages::TEXTURE_BINDING)
}

fn d3d12_texture_format(format: i32) -> Result<wgpu::TextureFormat, &'static str> {
    match format {
        D3D12_TEXTURE_FORMAT_NV12 => Ok(wgpu::TextureFormat::NV12),
        D3D12_TEXTURE_FORMAT_P010 => Ok(wgpu::TextureFormat::P010),
        D3D12_TEXTURE_FORMAT_BGRA8_UNORM => Ok(wgpu::TextureFormat::Bgra8Unorm),
        D3D12_TEXTURE_FORMAT_RGBA16_FLOAT => Ok(wgpu::TextureFormat::Rgba16Float),
        _ => Err("unsupported wgpu-d3d12 texture format"),
    }
}

fn cv_plane_layout_entry(binding: u32) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Texture {
            sample_type: wgpu::TextureSampleType::Float { filterable: true },
            view_dimension: wgpu::TextureViewDimension::D2,
            multisampled: false,
        },
        count: None,
    }
}

fn create_composite_bind_group_layout(device: &wgpu::Device) -> wgpu::BindGroupLayout {
    device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("VoidPlayer wgpu-d3d12 composite bind group layout"),
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
    })
}

fn create_composite_pipeline(
    device: &wgpu::Device,
    pipeline_layout: &wgpu::PipelineLayout,
    shader: &wgpu::ShaderModule,
    output_format: wgpu::TextureFormat,
    label: &'static str,
) -> wgpu::RenderPipeline {
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some(label),
        layout: Some(pipeline_layout),
        vertex: wgpu::VertexState {
            module: shader,
            entry_point: Some("vs_main"),
            buffers: &[],
            compilation_options: wgpu::PipelineCompilationOptions::default(),
        },
        fragment: Some(wgpu::FragmentState {
            module: shader,
            entry_point: Some("fs_main"),
            targets: &[Some(wgpu::ColorTargetState {
                format: output_format,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
            compilation_options: wgpu::PipelineCompilationOptions::default(),
        }),
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        multiview_mask: None,
        cache: None,
    })
}

fn cv_bind_entry<'a>(
    binding: u32,
    slot: usize,
    views: &'a [Option<wgpu::TextureView>; MAX_TRACKS],
    dummy: &'a wgpu::TextureView,
) -> wgpu::BindGroupEntry<'a> {
    wgpu::BindGroupEntry {
        binding,
        resource: wgpu::BindingResource::TextureView(views[slot].as_ref().unwrap_or(dummy)),
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
    decision: &D3D12PresentDecisionInfo,
    width: i32,
    height: i32,
    storage: i32,
    output_color_mode: i32,
    bytes: &mut Vec<u8>,
) {
    bytes.clear();
    push_vec4_f32(
        bytes,
        [
            width as f32,
            height as f32,
            decision.mode as f32,
            decision.track_count.max(1).min(MAX_TRACKS as i32) as f32,
        ],
    );
    push_vec4_f32(bytes, [decision.split_pos, storage as f32, 0.0, 0.0]);
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
    push_vec4_i32(bytes, [0, 0, 0, 0]);
    push_vec4_i32(bytes, decision.color_transfer);
    push_vec4_i32(bytes, decision.color_primaries);
    push_vec4_i32(bytes, [output_color_mode, 1, 1, 0]);
}

fn write_storage_buffer(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    cache: &mut Option<wgpu::Buffer>,
    bytes: &[u8],
    label: &'static str,
) {
    let size = bytes.len().max(4) as u64;
    let recreate = cache.as_ref().map_or(true, |buffer| buffer.size() < size);
    if recreate {
        *cache = Some(device.create_buffer(&wgpu::BufferDescriptor {
            label: Some(label),
            size,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
    }
    if let Some(buffer) = cache.as_ref() {
        queue.write_buffer(buffer, 0, bytes);
    }
}

unsafe fn import_d3d12_resource(
    device: &wgpu::Device,
    resource: *mut core::ffi::c_void,
    format: wgpu::TextureFormat,
    usage: wgpu::TextureUsages,
    label: &'static str,
    size: wgpu::Extent3d,
    mip_level_count: u32,
    sample_count: u32,
) -> Result<wgpu::Texture, &'static str> {
    let Some(_hal_device) = (unsafe { device.as_hal::<wgpu_hal::api::Dx12>() }) else {
        return Err("wgpu-d3d12 raw texture import requires a D3D12 wgpu device");
    };
    let resource = unsafe {
        windows::Win32::Graphics::Direct3D12::ID3D12Resource::from_raw_borrowed(&resource)
    }
    .cloned()
    .ok_or("wgpu-d3d12 texture import received an invalid resource")?;
    let hal_texture = unsafe {
        wgpu_hal::dx12::Device::texture_from_raw(
            resource,
            format,
            wgpu::TextureDimension::D2,
            size,
            mip_level_count,
            sample_count,
        )
    };
    let desc = wgpu::TextureDescriptor {
        label: Some(label),
        size,
        mip_level_count,
        sample_count,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage,
        view_formats: &[],
    };
    Ok(unsafe { device.create_texture_from_hal::<wgpu_hal::api::Dx12>(hal_texture, &desc) })
}

fn elapsed_us(start: Instant) -> u64 {
    start.elapsed().as_micros().min(u128::from(u64::MAX)) as u64
}
