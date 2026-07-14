# Color UI Tests

Portable color and decode-path visual checks live here. These scripts should use
small fixtures from `resources/video/` so they can run on any dev machine.

The current portable coverage checks software decode vs preferred hardware
decode by capturing the same frame through the platform's final viewport path
and asserting that the BGRA screenshots stay visually close.

`hevc_fullrange_bt709_decode_mode_single_track_diff.csv` uses
`resources/video/mhw_hevc_fullrange_bt709_3s.mp4`, a short stream-copied HEVC
fixture cut from the local Monster Hunter Wilds sample with FFmpeg `-c copy`.
It remains `yuv420p(pc, bt709)` and is meant to cover the full-range path that
ordinary bundled fixtures do not.

Keep `ui_tests/local/` probes only for large or private samples that cannot be
reduced into a portable fixture.

Windows native compositor policy coverage:

- `windows_native_auto_sdr_policy_smoke.csv` proves that Auto keeps an SDR
  fixture on the native BGRA8/G22 path even if Windows HDR is enabled.
- `windows_native_forced_scrgb_smoke.csv` runs with
  `VOIDPLAYER_WINDOWS_PRESENTATION_MODE=native-compositor-scrgb` and proves the
  RGBA16F/G10 final output while Flutter remains BGRA8 premultiplied sRGB.

Real PQ/HLG Auto promotion depends on an HDR-capable display and source, so the
machine-local `ui_tests/local/windows_native_auto_dolby_hdr_smoke.csv` and
promotion/demotion script cover that hardware-dependent boundary.
