# Color UI Tests

Portable color and decode-path visual checks live here. These scripts should use
small fixtures from `resources/video/` so they can run on any dev machine.

The current portable coverage checks software decode vs preferred hardware
decode by capturing the same frame through the full Flutter texture path and
asserting that the final BGRA screenshots stay visually close.

`hevc_fullrange_bt709_decode_mode_single_track_diff.csv` uses
`resources/video/mhw_hevc_fullrange_bt709_3s.mp4`, a short stream-copied HEVC
fixture cut from the local Monster Hunter Wilds sample with FFmpeg `-c copy`.
It remains `yuv420p(pc, bt709)` and is meant to cover the full-range path that
ordinary bundled fixtures do not.

Keep `ui_tests/local/` probes only for large or private samples that cannot be
reduced into a portable fixture.
