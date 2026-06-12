#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

MODE="${1:-debug}"
case "$MODE" in
  debug)
    ENGINE_NAME="${VOIDPLAYER_FLUTTER_LOCAL_ENGINE:-host_debug_unopt_arm64}"
    ;;
  release)
    ENGINE_NAME="${VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE:-host_release_arm64}"
    ;;
  *)
    echo "Usage: $0 [debug|release]" >&2
    exit 2
    ;;
esac

ENGINE_SRC="${VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH:-}"
if [[ -z "$ENGINE_SRC" ]]; then
  FLUTTER_BIN="${VOIDPLAYER_FLUTTER_BIN:-}"
  if [[ -n "$FLUTTER_BIN" ]]; then
    FLUTTER_ROOT="$(cd "$(dirname "$FLUTTER_BIN")/.." && pwd)"
    ENGINE_SRC="$FLUTTER_ROOT/engine/src"
  fi
fi

if [[ -z "$ENGINE_SRC" || ! -d "$ENGINE_SRC/out/$ENGINE_NAME" ]]; then
  echo "ERROR: local Flutter engine output not found for $ENGINE_NAME." >&2
  echo "Set VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH or VOIDPLAYER_FLUTTER_BIN." >&2
  exit 1
fi

FORK_REF="$(
  python3 - <<'PY'
import json
from pathlib import Path

lock = json.loads(Path("toolchains/flutter.lock.json").read_text(encoding="utf-8"))
print(lock["forkRef"])
PY
)"

OUT_DIR="${VOIDPLAYER_FLUTTER_ENGINE_ARCHIVE_DIR:-build/flutter-engine-artifacts}"
mkdir -p "$OUT_DIR"

ARCHIVE="$OUT_DIR/${FORK_REF}-macos-${ENGINE_NAME}.tar.gz"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/voidplayer-flutter-engine.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

SRC="$ENGINE_SRC/out/$ENGINE_NAME"
DST="$TMP/$ENGINE_NAME"
mkdir -p "$DST"

copy_if_exists() {
  local name="$1"
  if [[ -e "$SRC/$name" ]]; then
    rsync -a "$SRC/$name" "$DST/"
  fi
}

for name in \
  artifacts_arm64 \
  artifacts_x64 \
  universal \
  FlutterMacOS.framework \
  FlutterMacOS.xcframework \
  FlutterEmbedder.framework \
  dart-sdk \
  flutter_patched_sdk \
  flutter_patched_sdk_product \
  icudtl.dat \
  gen_snapshot \
  gen_snapshot_host_targeting_host \
  libFlutterMacOS.dylib \
  libflutter_engine.dylib \
  libimpeller.dylib \
  libpath_ops.dylib \
  libtessellator.dylib \
  libEGL.dylib \
  libGLESv2.dylib \
  libvulkan.dylib \
  libvk_swiftshader.dylib \
  libVkICD_mock_icd.dylib \
  vk_swiftshader_icd.json \
  VkICD_mock_icd.json \
  LICENSE \
  vm_platform.dill \
  vm_outline.dill \
  font-subset; do
  copy_if_exists "$name"
done

if compgen -G "$SRC/gen/*.snapshot" > /dev/null; then
  mkdir -p "$DST/gen"
  rsync -a "$SRC"/gen/*.snapshot "$DST/gen/"
fi

if [[ -d "$SRC/gen/flutter/lib/snapshot" ]]; then
  mkdir -p "$DST/gen/flutter/lib"
  rsync -a "$SRC/gen/flutter/lib/snapshot" "$DST/gen/flutter/lib/"
fi

if [[ -d "$SRC/clang_x64" ]]; then
  mkdir -p "$DST/clang_x64"
  for name in gen_snapshot icudtl.dat; do
    if [[ -e "$SRC/clang_x64/$name" ]]; then
      rsync -a "$SRC/clang_x64/$name" "$DST/clang_x64/"
    fi
  done
fi

COPYFILE_DISABLE=1 tar -C "$TMP" -czf "$ARCHIVE" "$ENGINE_NAME"
shasum -a 256 "$ARCHIVE" | tee "$ARCHIVE.sha256"
du -sh "$ARCHIVE"
