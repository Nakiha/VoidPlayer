#![deny(unsafe_op_in_unsafe_fn)]

use std::os::raw::c_int;

#[cfg(target_os = "windows")]
mod d3d12;
#[cfg(target_os = "macos")]
mod metal;

#[no_mangle]
pub extern "C" fn VPWgpuFfiVersion() -> c_int {
    voidplayer_wgpu_core::ABI_VERSION
}
