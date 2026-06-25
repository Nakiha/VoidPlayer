#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct OverlayRect {
    pub rect_uv0: u32,
    pub rect_uv1: u32,
    pub color_bgra: u32,
    pub track_idx: u32,
}
