# VoidPlayerCli

`VoidPlayerCli.exe` is shipped next to `void_player.exe` for agents, support
scripts, and manual debugging. Inspection commands are read-only. Generation
commands write VAC2/VACHUNK cache files under the explicit `--cache-root` and
`--hash` selected by the caller.

The first version focuses on the current analysis cache formats and the
headless generation path:

- VAC2 base cache: `cache/<hash>/base.vac`
- VACHUNK overlay chunks: `cache/<hash>/chunks/overlay/*.vck`
- VAC2 generation through the native base generator
- VACHUNK overlay generation by launching `tools/ffmpeg-analysis/void_ffmpeg_analyzer.exe`

## Commands

```text
VoidPlayerCli inspect <base.vac|chunk.vck> [--json] [--limit N]
VoidPlayerCli check <base.vac|chunk.vck> [--json]
VoidPlayerCli frame <base.vac> --index N [--json]
VoidPlayerCli chunk-frame <chunk.vck> --frame N [--json] [--limit N]
VoidPlayerCli benchmark-overlay <chunk.vck> --frame N [--width N] [--height N] [--iterations N] [--mode bitrate|qp] [--with-grid] [--json]
VoidPlayerCli benchmark-overlay-gpu <chunk.vck> --frame N [--width N] [--height N] [--iterations N] [--mode bitrate|qp] [--with-grid] [--json]
VoidPlayerCli generate-base --input <video> --cache-root <dir> --hash <hash> [--json]
VoidPlayerCli generate-overlay --input <video> --cache-root <dir> --hash <hash> --start-frame N --end-frame N [--codec h264|hevc|vvc] [--analyzer <exe>] [--json]
```

## Examples

Inspect a base cache:

```powershell
.\VoidPlayerCli.exe inspect "$env:APPDATA\VoidPlayer\cache\<hash>\base.vac"
```

Get stable machine-readable output for an agent:

```powershell
.\VoidPlayerCli.exe inspect "$env:APPDATA\VoidPlayer\cache\<hash>\base.vac" --json
.\VoidPlayerCli.exe frame "$env:APPDATA\VoidPlayer\cache\<hash>\base.vac" --index 128 --json
```

Inspect an overlay chunk and one frame's CU records:

```powershell
.\VoidPlayerCli.exe inspect "$env:APPDATA\VoidPlayer\cache\<hash>\chunks\overlay\<chunk>.vck"
.\VoidPlayerCli.exe chunk-frame "$env:APPDATA\VoidPlayer\cache\<hash>\chunks\overlay\<chunk>.vck" --frame 128 --json --limit 20
```

Benchmark one overlay frame's heatmap raster path:

```powershell
.\VoidPlayerCli.exe benchmark-overlay `
  "$env:APPDATA\VoidPlayer\cache\<hash>\chunks\overlay\<chunk>.vck" `
  --frame 128 `
  --iterations 240 `
  --mode bitrate `
  --with-grid `
  --json
```

This command measures CPU rasterization into a BGRA overlay buffer. It does not
include GUI texture upload, D3D blending, or compositor work. With
`--with-grid`, it also rasters the CU/MB boundary mask and reports the estimated
color/mask upload bytes for one frame.

Benchmark the D3D11 overlay passes for the same frame:

```powershell
.\VoidPlayerCli.exe benchmark-overlay-gpu `
  "$env:APPDATA\VoidPlayer\cache\<hash>\chunks\overlay\<chunk>.vck" `
  --frame 128 `
  --iterations 240 `
  --mode bitrate `
  --with-grid `
  --json
```

The GPU command uses timestamp queries and reports rect upload CPU wall time,
color instance pass, GPU mask materialization pass, fullscreen invert pass, and
full overlay pass averages, plus whether the benchmark used hardware D3D11 or
WARP. It creates a hardware D3D11 device when available and falls back to WARP
if needed.

`inspect <chunk.vck> --json` reports the chunk header compression algorithm and
per-section `compressed`, `size`, and `decodedSize` fields. Current published
overlay chunks may use zstd on large sections; CLI readers transparently
decompress before returning frame or CU records.

Generate cache without opening the GUI:

```powershell
$cacheRoot = "$env:APPDATA\VoidPlayer\cache"
$hash = "<sha256-or-debug-id>"

.\VoidPlayerCli.exe generate-base `
  --input "D:\media\input.mp4" `
  --cache-root $cacheRoot `
  --hash $hash `
  --json

.\VoidPlayerCli.exe generate-overlay `
  --input "D:\media\input.mp4" `
  --cache-root $cacheRoot `
  --hash $hash `
  --start-frame 128 `
  --end-frame 191 `
  --codec hevc `
  --json
```

`generate-overlay` looks for `void_ffmpeg_analyzer.exe` in this order:

1. `--analyzer <exe>`
2. `VOID_FFMPEG_ANALYZER`
3. `tools/ffmpeg-analysis/void_ffmpeg_analyzer.exe` next to the installed GUI

The analyzer writes a temporary uncompressed `.vck`; `generate-overlay` validates
and republishes it through the same native VACHUNK writer used by the GUI, so
generated cache files follow the current section-compression policy.

## Exit Codes

| Code | Meaning |
| --- | --- |
| `0` | The command succeeded. |
| `1` | CLI usage error, such as a missing required argument. |
| `2` | The file could not be opened, had an unknown magic, or failed validation. |

## Test Coverage

The native analysis test flow includes an integration smoke for this tool. The
test generates a small VAC2 base file and VACHUNK overlay chunk, then runs:

- `inspect --json`
- `check --json`
- `frame --json`
- `chunk-frame --json`
- `benchmark-overlay --json`
- `benchmark-overlay-gpu --json`
- `generate-base --json`
- `generate-overlay --json`

This is intentional. When the cache format moves to VAC3 or when VACHUNK starts
using compressed sections, update `VoidPlayerCli.exe` in the same change so the
analysis tests keep external-agent tooling honest.
