# VoidPlayer Quality Analysis Process Protocol v1

Status: **implemented for `score-quality --jsonl` lifecycle records**. The
frame/report payload remains defined by `quality-output-v5.schema.json`; optional
tile evidence is defined by `quality-tile-v1.schema.json`. This document defines
how one CLI process is started, observed, cancelled and terminated.

## 1. Scope and transport

Protocol v1 uses **one immutable analysis request per operating-system process**.
It is intentionally not a daemon protocol. The caller supplies the request as CLI
arguments, reads UTF-8 JSON Lines from stdout, and reads diagnostics from stderr.

- stdout MUST contain JSON/JSONL only when `--json` or `--jsonl` is used.
- native logs, warnings that are not protocol records, and crash diagnostics MUST
  go to stderr.
- each JSONL record MUST occupy exactly one line and MUST be flushed promptly.
- the process MUST NOT accept a second request.
- `--json` remains a non-streaming compatibility mode and emits exactly one v5
  report or error object. Lifecycle records are emitted only by `--jsonl`.

Protocol lifecycle and quality payload versions are independent:

- `protocolVersion: 1` governs session/progress/completion sequencing;
- `schemaId: quality-output-v5` governs report, frame sample and error payloads;
- `tileSchemaId: quality-tile-v1` governs optional per-sample tile evidence;
- `metricVersion` governs the numerical algorithm.

Changing progress fields does not by itself require a metric version change.
Changing a score's meaning does.

## 2. JSONL state machine

A successful stream has this exact grammar:

```text
qualitySession
qualityProgress*          # opening/decoding/finalizing, monotonic sequence
qualityReport             # quality-output-v5
(qualityFrameSample       # quality-output-v5, ascending sample order
 qualityTileSample?)*     # quality-tile-v1, same sampleIndex when requested
qualityEvent*             # quality-event-v1 candidate evidence
qualityComplete           # the only successful terminal record
EOF
```

A failed or cancelled stream has this grammar:

```text
qualitySession?
qualityProgress*
qualityError              # quality-output-v5; the only failed terminal record
EOF
```

`qualitySession` is absent only when arguments cannot be parsed or validated far
enough to establish a request. No record is permitted after a terminal record.
Consumers MUST use `type`, not line number, to dispatch records. Unknown record
types from a newer minor-compatible producer SHOULD be ignored.

`qualityEvent` uses `quality-event-v1`. Thresholding, temporal grouping, spatial
matching and de-duplication belong to the CLI; the GUI MUST NOT reproduce those
decisions. Events are experimental candidates rather than calibrated pass/fail
labels and a successful stream may emit zero events.

## 3. Session record

`qualitySession` describes the request before media open/decode begins:

```json
{
  "type": "qualitySession",
  "protocolVersion": 1,
  "requestId": "q-...",
  "inputIdentity": {
    "kind": "local-file-stat-v1",
    "normalizedPath": "D:/media/input.mp4",
    "sizeBytes": 123456,
    "mtimeToken": "...",
    "digest": "fnv1a64:..."
  },
  "resultConfig": {
    "metricVersion": "quality-demo-v5",
    "metrics": ["blockiness", "banding"],
    "regionOutput": "summary",
    "tileOutput": "none",
    "events": "candidates",
    "eventPolicyVersion": "quality-candidate-policy-v1",
    "sampleIntervalUs": 1000000,
    "maxSamples": null
  },
  "executionConfig": {
    "backend": "cpu",
    "cpuMode": "auto",
    "decodeThreads": 0,
    "cpuWorkers": 0,
    "cpuInFlight": 0
  },
  "resultKey": "fnv1a64:..."
}
```

`requestId` is correlation only and MUST NOT participate in caching. A caller may
provide a 1-128 character visible-ASCII `--request-id` without spaces; otherwise
the CLI generates one. It MUST be echoed by all
v1 lifecycle records. Payload-v5 records are correlated by their position between
the session and terminal record; v1 deliberately does not mutate the closed v5
payload schema merely to add correlation fields.

### 3.1 Input identity and cache correctness

`local-file-stat-v1` is a fast, local cache identity, not a content hash.
`normalizedPath`, `sizeBytes` and `mtimeToken` form the identity. `mtimeToken` is
the native filesystem clock tick value serialized as a decimal string; it is
opaque outside the current host/toolchain and MUST NOT be interpreted as Unix
time. Missing/unreadable input fails before `qualitySession`.

`digest` and `resultKey` use FNV-1a 64 only as compact lookup keys. A cache MUST
store and compare the complete `inputIdentity` and `resultConfig` objects after a
digest hit. A digest match alone MUST NOT be treated as a valid cache hit.

Future callers that already own a full content SHA-256 may introduce a new input
identity kind in a later protocol version. It must not silently change the
semantics of `local-file-stat-v1`.

### 3.2 Result versus execution configuration

`resultConfig` contains fields that can change the returned evidence and therefore
participates in `resultKey`. Metric names are serialized in canonical order.

`executionConfig` contains scheduling/backend choices. It is recorded for audit
and parity diagnosis but does not participate in `resultKey`. Cache reuse across
different execution configs is allowed only while backend parity remains part of
the quality algorithm contract. A parity-breaking backend must receive a distinct
metric version or result-config discriminator.

The canonical result-key input is the UTF-8 concatenation below, without spaces:

```text
quality-result-v1\n
<input identity digest>\n
<metric version>\n
<comma-separated canonical metric names>\n
<region output>\n
<tile output>\n
<event mode>\n
<event policy version>\n
<sample interval microseconds>\n
<max samples, or null>
```

## 4. Progress record

```json
{
  "type": "qualityProgress",
  "protocolVersion": 1,
  "requestId": "q-...",
  "sequence": 2,
  "phase": "decoding",
  "packetCount": 180,
  "packetBytes": 8388608,
  "decodedFrames": 174,
  "sampledFrames": 6,
  "ptsUs": 5000000,
  "durationUs": 24768000
}
```

- `sequence` starts at zero and strictly increases within one process.
- phases are ordered `opening` -> `decoding` -> `finalizing`; a phase may emit
  multiple records and an empty/failed input may skip later phases.
- counters MUST be monotonic. `ptsUs` may be null before the first decoded frame.
- `durationUs` may be null when the container does not provide a duration.
- protocol v1 does not expose a synthetic percentage. Consumers may derive a
  tentative ratio from PTS/duration, but MUST tolerate non-monotonic timestamps,
  unknown duration and early `maxSamples` truncation.
- producers SHOULD bound progress frequency; consumers MUST NOT depend on an
  exact record count.

### 4.1 Tile sample records

`--tiles full --jsonl` emits one `qualityTileSample` immediately after every
`qualityFrameSample`. `--tiles none` is the default. Tile output is deliberately
JSONL-only so a large video cannot silently inflate the single-object `--json`
compatibility payload. Explicit `--backend wgpu` is rejected for tile requests;
`auto` resolves to CPU because the current WGPU backend exposes frame aggregates
only.

All selected metrics use the same approximately 64 x 64 decoded-luma grid. The
grid is balanced across each axis: for column `c`, pixel bounds are
`floor(c * frameWidth / columns)` through
`floor((c + 1) * frameWidth / columns)`, and rows use the analogous formula.
Values are row-major and remain in `[0, 1]`, with higher values meaning worse.
This avoids undersized sliver tiles at the right and bottom edges.

The four spatial proxies are recomputed on each local tile. Flicker values are
three-frame local luma curvature on the same grid and are `available: false`
with `values: null` for the first two decoded frames, incompatible geometry, or
a scene cut. A valid array may contain `null` only when an individual tile is
too small for that metric. Frame aggregates remain authoritative and are not
defined as an arithmetic mean of tile values because their robust/global
aggregation differs.

Formal schema: [quality-tile-v1.schema.json](quality-tile-v1.schema.json).

## 5. Candidate event records

`--events candidates` (the JSONL default) emits `qualityEvent` records after all
frame samples. `--events none` disables aggregation. Event policy v1 has two
classifications:

- `spatialCandidate`: currently banding only. The event retains the strongest
  detected region on its peak frame. Every detected region becomes its own
  candidate track; consecutive samples merge one-to-one only when their regions
  overlap (IoU >= 0.10), so multiple simultaneous defects remain separate and
  unrelated areas never become one full-frame rectangle.
- `relativeOutlier`: a within-video candidate for metrics without usable spatial
  evidence. At least five valid samples are required. The threshold is
  `max(P90, median + 3 * 1.4826 * MAD)` and the complete median/MAD/P90 evidence
  is serialized. Constant series produce no events.

Relative events have `region: null`; downstream code MUST create a time-only mark,
not a full-frame rectangle. Spatial events are emitted only with `--regions full`.
`summary` and `none` continue to permit relative events but do not leak hidden
region details. `calibrated: false` is normative: neither classification is an
absolute quality verdict.

Formal schema: [quality-event-v1.schema.json](quality-event-v1.schema.json).

## 6. Completion and error records

`qualityComplete` confirms that every preceding report/frame/tile/event record
was written successfully:

```json
{
  "type": "qualityComplete",
  "protocolVersion": 1,
  "requestId": "q-...",
  "status": "success",
  "reportRecords": 1,
  "frameSampleRecords": 10,
  "tileSampleRecords": 10,
  "eventRecords": 0
}
```

`tileSampleRecords` is zero when tile output is disabled and otherwise MUST equal
`frameSampleRecords`.

The consumer MUST NOT commit a streamed result to cache until it receives a
matching `qualityComplete`. EOF after a report but before completion is an
incomplete/aborted transaction.

Failures use the existing v5 `qualityError` envelope. Stable v1 codes include:

- if no `qualitySession` was emitted, `protocolVersion` and `requestId` are
  omitted;
- if a `qualitySession` was emitted, the terminal error MUST carry
  `protocolVersion: 1` and the same `requestId` as that session.

| code | meaning | exit code |
| --- | --- | ---: |
| `invalid_arguments` and other validation codes | request rejected | 1 |
| `analysis_failed` | open/decode/metric failure | 2 |
| `input_changed` | local input identity changed after session start | 2 |
| `cancelled` | cooperative cancellation observed | 130 |

Additional codes may be added. Consumers MUST branch on `code`, never localized
`message` text.

## 7. Cancellation

Cancellation is cooperative and idempotent:

- POSIX callers send `SIGINT` or `SIGTERM` to the child process;
- Windows callers send `CTRL_C_EVENT` or `CTRL_BREAK_EVENT` to a child created in
  a console process group;
- the handler only sets a cancellation flag;
- media open/read observes that flag through FFmpeg's interrupt callback;
- decode and metric scheduling check it at bounded work boundaries;
- the CLI emits one `qualityError` with `code: cancelled` and exits 130.

If a frontend cannot deliver a cooperative signal, it may terminate the process.
That path intentionally has no terminal record and MUST be treated as an aborted,
non-cacheable transaction. Hard termination is not reported as `analysis_failed`.

## 8. Compatibility rules

1. Consumers MUST reject unsupported `protocolVersion` values on lifecycle
   records, but may continue to validate standalone v5 payloads.
2. New optional lifecycle fields are backward compatible. Removing/renaming a
   required field, changing ordering, or changing terminal semantics requires a
   protocol major version.
3. A new quality payload schema does not require a protocol bump if record roles
   and transaction semantics are unchanged.
4. A numerical metric change requires a new `metricVersion` and therefore a new
   `resultKey` even if its JSON shape is unchanged.
5. stdout contamination is a protocol violation; stderr content is never parsed
   as protocol data.

Formal lifecycle-record schema:
[quality-process-v1.schema.json](quality-process-v1.schema.json). Quality report,
frame sample and error records continue to use
[quality-output-v5.schema.json](quality-output-v5.schema.json). Tile records use
[quality-tile-v1.schema.json](quality-tile-v1.schema.json).
