#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

MODES="${VOIDPLAYER_FLUTTER_ENGINE_MODES:-debug}"
FLUTTER_BIN="${VOIDPLAYER_FLUTTER_BIN:-$ROOT/.toolchains/flutter/bin/flutter}"
FLUTTER_ROOT="$(cd "$(dirname "$FLUTTER_BIN")/.." && pwd)"
ENGINE_SRC="$FLUTTER_ROOT/engine/src"
ENGINE_OUT="$ENGINE_SRC/out"
mkdir -p "$ENGINE_OUT"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/voidplayer-flutter-engine-download.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

python3 - "$MODES" <<'PY' > "$TMP/artifacts.tsv"
import json
import sys
from pathlib import Path

modes = {item.strip() for item in sys.argv[1].split(",") if item.strip()}
if "all" in modes:
    modes = {"debug", "release"}

lock = json.loads(Path("toolchains/flutter.lock.json").read_text(encoding="utf-8"))
repo = lock["forkRemote"].removeprefix("https://github.com/").removesuffix(".git")
release_tag = lock.get("macosLocalEngineReleaseTag") or lock["forkRef"]
artifacts = lock.get("macosLocalEngineArtifacts", {})

for mode in ("debug", "release"):
    if mode not in modes:
        continue
    spec = artifacts.get(mode)
    if not spec:
        raise SystemExit(f"missing macosLocalEngineArtifacts.{mode} in toolchain lock")
    print(
        "\t".join(
            [
                mode,
                repo,
                release_tag,
                spec["asset"],
                spec["sha256"],
                spec["engine"],
                spec.get("host", spec["engine"]),
            ]
        )
    )
PY

while IFS=$'\t' read -r mode repo ref asset sha256 engine host; do
  echo "Downloading Flutter macOS local engine: $asset"
  gh release download "$ref" -R "$repo" --pattern "$asset" --dir "$TMP" --clobber
  actual="$(shasum -a 256 "$TMP/$asset" | awk '{print $1}')"
  if [[ "$actual" != "$sha256" ]]; then
    echo "ERROR: sha256 mismatch for $asset" >&2
    echo "  expected: $sha256" >&2
    echo "  actual:   $actual" >&2
    exit 1
  fi
  tar -C "$ENGINE_OUT" -xzf "$TMP/$asset"
  if [[ ! -d "$ENGINE_OUT/$engine" ]]; then
    echo "ERROR: archive $asset did not produce $ENGINE_OUT/$engine" >&2
    exit 1
  fi

  if [[ "$mode" == "debug" ]]; then
    export VOIDPLAYER_FLUTTER_LOCAL_ENGINE="$engine"
    export VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST="$host"
  else
    export VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE="$engine"
    export VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE="$host"
  fi
done < "$TMP/artifacts.tsv"

export VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH="$ENGINE_SRC"

if [[ -n "${GITHUB_ENV:-}" ]]; then
  {
    echo "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH=$VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH"
    if [[ -n "${VOIDPLAYER_FLUTTER_LOCAL_ENGINE:-}" ]]; then
      echo "VOIDPLAYER_FLUTTER_LOCAL_ENGINE=$VOIDPLAYER_FLUTTER_LOCAL_ENGINE"
      echo "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST=$VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST"
    fi
    if [[ -n "${VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE:-}" ]]; then
      echo "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE=$VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE"
      echo "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE=$VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE"
    fi
  } >> "$GITHUB_ENV"
fi

echo "Flutter macOS local engine ready: $ENGINE_SRC"
