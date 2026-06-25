#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use voidplayer_wgpu_core::{
    composite_metal_retained_source_with_renderer,
    render_metal_cv_pixel_buffer_frame_set_with_renderer, render_metal_package,
    render_metal_package_with_renderer, WgpuMetalCVPixelBufferRenderRequest,
    WgpuMetalRenderRequest, WgpuMetalRenderer, WgpuMetalRetainedCompositeRequest, ABI_VERSION,
};

#[repr(C)]
pub struct VPWgpuMetalRendererInfo {
    adapter_description: [c_char; 128],
    driver_type: [c_char; 64],
    backend: [c_char; 32],
    device_type: [c_char; 32],
    vendor_id: u32,
    device_id: u32,
    supports_texture_format_16bit_norm: u32,
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
pub extern "C" fn VPWgpuFfiVersion() -> c_int {
    ABI_VERSION
}

#[no_mangle]
pub extern "C" fn VPWgpuMetalRendererCreate(
    error: *mut c_char,
    error_size: usize,
) -> *mut WgpuMetalRenderer {
    let result = catch_unwind(AssertUnwindSafe(WgpuMetalRenderer::new));
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
            write_error(error, error_size, "wgpu-metal renderer create panicked");
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn VPWgpuMetalRendererGetInfo(
    renderer: *mut WgpuMetalRenderer,
    info: *mut VPWgpuMetalRendererInfo,
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
                "wgpu-metal adapter"
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
        info_ref.supports_texture_format_16bit_norm =
            renderer_ref.supports_texture_format_16bit_norm() as u32;
    }));
    if result.is_err() {
        return -1;
    }
    0
}

#[no_mangle]
pub extern "C" fn VPWgpuMetalRendererDestroy(renderer: *mut WgpuMetalRenderer) {
    if renderer.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(renderer));
    }
}

#[no_mangle]
pub extern "C" fn VPWgpuMetalRendererRenderPackage(
    renderer: *mut WgpuMetalRenderer,
    request: *const WgpuMetalRenderRequest,
) -> c_int {
    if renderer.is_null() || request.is_null() {
        return -1;
    }
    let renderer_ref = unsafe { &mut *renderer };
    let request_ref = unsafe { &*request };
    let result = catch_unwind(AssertUnwindSafe(|| {
        render_metal_package_with_renderer(renderer_ref, request_ref)
    }));
    match result {
        Err(_) => {
            write_error(
                request_ref.error,
                request_ref.error_size,
                "wgpu-metal render panicked",
            );
            -1
        }
        Ok(Ok(())) => {
            write_error(request_ref.error, request_ref.error_size, "");
            0
        }
        Ok(Err(message)) => {
            write_error(request_ref.error, request_ref.error_size, message);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn VPWgpuMetalRendererRenderCVPixelBufferFrameSet(
    renderer: *mut WgpuMetalRenderer,
    request: *const WgpuMetalCVPixelBufferRenderRequest,
) -> c_int {
    if renderer.is_null() || request.is_null() {
        return -1;
    }
    let renderer_ref = unsafe { &mut *renderer };
    let request_ref = unsafe { &*request };
    let result = catch_unwind(AssertUnwindSafe(|| {
        render_metal_cv_pixel_buffer_frame_set_with_renderer(renderer_ref, request_ref)
    }));
    match result {
        Err(_) => {
            write_error(
                request_ref.error,
                request_ref.error_size,
                "wgpu-metal CVPixelBuffer render panicked",
            );
            -1
        }
        Ok(Ok(())) => {
            write_error(request_ref.error, request_ref.error_size, "");
            0
        }
        Ok(Err(message)) => {
            write_error(request_ref.error, request_ref.error_size, message);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn VPWgpuMetalRendererCompositeRetainedSource(
    renderer: *mut WgpuMetalRenderer,
    request: *const WgpuMetalRetainedCompositeRequest,
) -> c_int {
    if renderer.is_null() || request.is_null() {
        return -1;
    }
    let renderer_ref = unsafe { &mut *renderer };
    let request_ref = unsafe { &*request };
    let result = catch_unwind(AssertUnwindSafe(|| {
        composite_metal_retained_source_with_renderer(renderer_ref, request_ref)
    }));
    match result {
        Err(_) => {
            write_error(
                request_ref.error,
                request_ref.error_size,
                "wgpu-metal retained composite panicked",
            );
            -1
        }
        Ok(Ok(())) => {
            write_error(request_ref.error, request_ref.error_size, "");
            0
        }
        Ok(Err(message)) => {
            write_error(request_ref.error, request_ref.error_size, message);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn VPWgpuMetalRenderPackage(request: *const WgpuMetalRenderRequest) -> c_int {
    if request.is_null() {
        return -1;
    }
    let request_ref = unsafe { &*request };
    let result = catch_unwind(AssertUnwindSafe(|| render_metal_package(request_ref)));
    match result {
        Err(_) => {
            write_error(
                request_ref.error,
                request_ref.error_size,
                "wgpu-metal render panicked",
            );
            -1
        }
        Ok(Ok(())) => {
            write_error(request_ref.error, request_ref.error_size, "");
            0
        }
        Ok(Err(message)) => {
            write_error(request_ref.error, request_ref.error_size, message);
            -1
        }
    }
}
