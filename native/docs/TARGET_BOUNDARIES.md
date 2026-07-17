# Native Target Boundaries

## Active Targets

| Target | Ownership |
| --- | --- |
| `void_player_portable_core` | Shared clock, seek, queues, layout and presentation contracts |
| `void_media_ffmpeg` | Shared FFmpeg demux/decode policy |
| `void_renderer_portable_driver` | Shared renderer scheduler used by native smokes and macOS |
| `void_macos_native_player` | macOS player bridge, VideoToolbox and Metal presentation |
| `video_renderer_lib` (Windows) | Shared renderer plus D3D11VA provider/shared snapshot foundation；presentation factory remains fail-closed |
| `analysis_lib` / `VoidPlayerCli` | Optional analysis cache and CLI when `BUILD_ANALYSIS=ON` |
| `video_renderer_native` | Optional local Python tooling when `BUILD_PYTHON=ON` |
| `macos_*_smoke` and portable smoke targets | CTest-only validation |

`native/windows/presentation/windows_presentation_backend.*` is a fail-closed factory
boundary. It is not an active renderer target.

## Options

| Option | Behavior |
| --- | --- |
| `BUILD_ANALYSIS` | Enables analysis library, CLI and analysis tests |
| `BUILD_PYTHON` | Enables the optional pybind11 tooling module |
| `BUILD_TESTS` | Enables CTest targets |

There is no renderer C FFI target or dist staging contract. Platform runners own their
channel bridges. Static libraries remain internal unless this document explicitly marks
them as a release surface.
