#![deny(unsafe_op_in_unsafe_fn)]

pub mod color;
#[cfg(target_os = "windows")]
pub mod d3d12;
pub mod import;
pub mod layout;
#[cfg(target_os = "macos")]
pub mod metal;
pub mod overlay;

pub const ABI_VERSION: i32 = 16;
pub const MAX_TRACKS: usize = 4;

pub const YUV_FORMAT_NV12: i32 = 1;
pub const YUV_FORMAT_P010: i32 = 2;
pub const YUV_FORMAT_YUV420P: i32 = 3;
pub const YUV_FORMAT_YUV420P10LE: i32 = 4;

#[cfg(target_os = "windows")]
pub use d3d12::*;
#[cfg(target_os = "macos")]
pub use metal::*;
