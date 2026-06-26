use std::time::Instant;
use windows::core::Interface;

pub const D3D12_TEXTURE_FORMAT_NV12: i32 = 1;
pub const D3D12_TEXTURE_FORMAT_P010: i32 = 2;
pub const D3D12_TEXTURE_FORMAT_BGRA8_UNORM: i32 = 3;
pub const D3D12_TEXTURE_FORMAT_RGBA16_FLOAT: i32 = 4;
pub const OUTPUT_COLOR_MODE_SDR: i32 = 1;
pub const OUTPUT_COLOR_MODE_EDR: i32 = 2;

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

pub struct WgpuD3D12Renderer {
    adapter_info: wgpu::AdapterInfo,
    device: wgpu::Device,
    _queue: wgpu::Queue,
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

        let adapter_info = adapter.get_info();
        Ok(Self {
            adapter_info,
            device,
            _queue: queue,
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
}

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

unsafe fn import_d3d12_resource(
    device: &wgpu::Device,
    resource: *mut core::ffi::c_void,
    format: wgpu::TextureFormat,
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
        label: Some("VoidPlayer imported D3D12VA frame"),
        size,
        mip_level_count,
        sample_count,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage: wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    };
    Ok(unsafe { device.create_texture_from_hal::<wgpu_hal::api::Dx12>(hal_texture, &desc) })
}

fn elapsed_us(start: Instant) -> u64 {
    start.elapsed().as_micros().min(u128::from(u64::MAX)) as u64
}
