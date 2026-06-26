#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MetalDestinationImport {
    pub mtl_texture: *mut core::ffi::c_void,
    pub width: i32,
    pub height: i32,
}
