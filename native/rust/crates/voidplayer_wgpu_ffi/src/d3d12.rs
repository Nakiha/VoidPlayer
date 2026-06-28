use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use voidplayer_wgpu_core::{
    WgpuD3D12CompositeRequest, WgpuD3D12ProfilerSnapshot, WgpuD3D12RenderTargetClearRequest,
    WgpuD3D12Renderer, WgpuD3D12TextureImportRequest,
};

#[repr(C)]
pub struct VPWgpuD3D12RendererInfo {
    adapter_description: [c_char; 128],
    driver_type: [c_char; 64],
    backend: [c_char; 32],
    device_type: [c_char; 32],
    vendor_id: u32,
    device_id: u32,
    supports_nv12: u32,
    supports_p010: u32,
    supports_rgba16_float: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct VPWgpuD3D12ProfilerSnapshot {
    destination_import_count: u64,
    source_import_count: u64,
    submit_count: u64,
    last_import_us: u64,
    last_prepare_us: u64,
    last_pass_encode_us: u64,
    last_submit_us: u64,
    last_cpu_render_us: u64,
    overlay_layer_rebuild_count: u64,
    overlay_layer_reuse_count: u64,
    overlay_buffer_write_count: u64,
    last_overlay_encode_us: u64,
}

impl From<WgpuD3D12ProfilerSnapshot> for VPWgpuD3D12ProfilerSnapshot {
    fn from(value: WgpuD3D12ProfilerSnapshot) -> Self {
        Self {
            destination_import_count: value.destination_import_count,
            source_import_count: value.source_import_count,
            submit_count: value.submit_count,
            last_import_us: value.last_import_us,
            last_prepare_us: value.last_prepare_us,
            last_pass_encode_us: value.last_pass_encode_us,
            last_submit_us: value.last_submit_us,
            last_cpu_render_us: value.last_cpu_render_us,
            overlay_layer_rebuild_count: value.overlay_layer_rebuild_count,
            overlay_layer_reuse_count: value.overlay_layer_reuse_count,
            overlay_buffer_write_count: value.overlay_buffer_write_count,
            last_overlay_encode_us: value.last_overlay_encode_us,
        }
    }
}

fn write_error(dst: *mut c_char, cap: usize, message: &str) {
    if dst.is_null() || cap == 0 {
        return;
    }
    let sanitized = message.replace('\0', " ");
    let c_string = CString::new(sanitized).unwrap_or_else(|_| CString::new("wgpu error").unwrap());
    let bytes = c_string.as_bytes_with_nul();
    let copy_len = bytes.len().min(cap);
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), dst.cast::<u8>(), copy_len);
        if copy_len == cap {
            *dst.add(cap - 1) = 0;
        }
    }
}

fn write_fixed_cstr<const N: usize>(dst: &mut [c_char; N], message: &str) {
    dst.fill(0);
    if N == 0 {
        return;
    }
    let sanitized = message.replace('\0', " ");
    let bytes = sanitized.as_bytes();
    let copy_len = bytes.len().min(N - 1);
    for (slot, byte) in dst.iter_mut().take(copy_len).zip(bytes.iter()) {
        *slot = *byte as c_char;
    }
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererCreate(
    error: *mut c_char,
    error_size: usize,
) -> *mut WgpuD3D12Renderer {
    let result = catch_unwind(AssertUnwindSafe(WgpuD3D12Renderer::new));
    match result {
        Ok(Ok(renderer)) => {
            write_error(error, error_size, "");
            Box::into_raw(Box::new(renderer))
        }
        Ok(Err(message)) => {
            write_error(error, error_size, message);
            ptr::null_mut()
        }
        Err(_) => {
            write_error(error, error_size, "wgpu-d3d12 renderer create panicked");
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererGetInfo(
    renderer: *mut WgpuD3D12Renderer,
    info: *mut VPWgpuD3D12RendererInfo,
) -> c_int {
    if renderer.is_null() || info.is_null() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer_ref = unsafe { &mut *renderer };
        let info_ref = unsafe { &mut *info };
        let adapter = renderer_ref.adapter_info();
        write_fixed_cstr(
            &mut info_ref.adapter_description,
            if adapter.name.is_empty() {
                "wgpu-d3d12 adapter"
            } else {
                &adapter.name
            },
        );
        let driver_type = if adapter.driver.is_empty() {
            format!("{:?}", adapter.device_type)
        } else {
            adapter.driver.clone()
        };
        write_fixed_cstr(&mut info_ref.driver_type, &driver_type);
        write_fixed_cstr(&mut info_ref.backend, &format!("{:?}", adapter.backend));
        write_fixed_cstr(
            &mut info_ref.device_type,
            &format!("{:?}", adapter.device_type),
        );
        info_ref.vendor_id = adapter.vendor;
        info_ref.device_id = adapter.device;
        info_ref.supports_nv12 = renderer_ref.supports_nv12() as u32;
        info_ref.supports_p010 = renderer_ref.supports_p010() as u32;
        info_ref.supports_rgba16_float = renderer_ref.supports_rgba16_float() as u32;
    }));
    if result.is_err() {
        return -1;
    }
    0
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererGetProfilerSnapshot(
    renderer: *mut WgpuD3D12Renderer,
    snapshot: *mut VPWgpuD3D12ProfilerSnapshot,
) -> c_int {
    if renderer.is_null() || snapshot.is_null() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer_ref = unsafe { &mut *renderer };
        let snapshot_ref = unsafe { &mut *snapshot };
        *snapshot_ref = renderer_ref.profiler_snapshot().into();
    }));
    if result.is_err() {
        return -1;
    }
    0
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererD3D12Device(
    renderer: *mut WgpuD3D12Renderer,
) -> *mut core::ffi::c_void {
    if renderer.is_null() {
        return ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer_ref = unsafe { &mut *renderer };
        renderer_ref.d3d12_device_ptr()
    }));
    result.unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererD3D12CommandQueue(
    renderer: *mut WgpuD3D12Renderer,
) -> *mut core::ffi::c_void {
    if renderer.is_null() {
        return ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer_ref = unsafe { &mut *renderer };
        renderer_ref.d3d12_command_queue_ptr()
    }));
    result.unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererImportTextureForProbe(
    renderer: *mut WgpuD3D12Renderer,
    request: *const WgpuD3D12TextureImportRequest,
) -> c_int {
    if renderer.is_null() || request.is_null() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer_ref = unsafe { &mut *renderer };
        let request_ref = unsafe { &*request };
        match renderer_ref.import_texture_for_probe(request_ref) {
            Ok(()) => {
                write_error(request_ref.error, request_ref.error_size, "");
                0
            }
            Err(message) => {
                write_error(request_ref.error, request_ref.error_size, message);
                -1
            }
        }
    }));
    result.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererClearRenderTargetForProbe(
    renderer: *mut WgpuD3D12Renderer,
    request: *const WgpuD3D12RenderTargetClearRequest,
) -> c_int {
    if renderer.is_null() || request.is_null() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer_ref = unsafe { &mut *renderer };
        let request_ref = unsafe { &*request };
        match renderer_ref.clear_render_target_for_probe(request_ref) {
            Ok(()) => {
                write_error(request_ref.error, request_ref.error_size, "");
                0
            }
            Err(message) => {
                write_error(request_ref.error, request_ref.error_size, message);
                -1
            }
        }
    }));
    result.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererRenderComposite(
    renderer: *mut WgpuD3D12Renderer,
    request: *const WgpuD3D12CompositeRequest,
) -> c_int {
    if renderer.is_null() || request.is_null() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let renderer_ref = unsafe { &mut *renderer };
        let request_ref = unsafe { &*request };
        match renderer_ref.render_composite(request_ref) {
            Ok(()) => {
                write_error(request_ref.error, request_ref.error_size, "");
                0
            }
            Err(message) => {
                write_error(request_ref.error, request_ref.error_size, message);
                -1
            }
        }
    }));
    result.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn VPWgpuD3D12RendererDestroy(renderer: *mut WgpuD3D12Renderer) {
    if renderer.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(renderer));
    }
}
