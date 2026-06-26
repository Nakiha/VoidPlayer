#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ColorMetadata {
    pub range: i32,
    pub matrix: i32,
    pub transfer: i32,
    pub primaries: i32,
}
