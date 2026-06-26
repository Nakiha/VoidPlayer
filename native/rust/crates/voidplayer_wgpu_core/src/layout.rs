#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct TrackLayout {
    pub display_offset_x: f32,
    pub display_offset_y: f32,
    pub inv_display_size_x: f32,
    pub inv_display_size_y: f32,
    pub view_offset_uv_x: f32,
    pub view_offset_uv_y: f32,
}
