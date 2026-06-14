#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

PYTHON_BIN="${PYTHON:-python3}"
TARGET="${VOIDPLAYER_FLUTTER_TOOLCHAIN_PATH:-.toolchains/flutter}"
case "$TARGET" in
  /*) TARGET_ABS="$TARGET" ;;
  *) TARGET_ABS="$ROOT/$TARGET" ;;
esac

export VOIDPLAYER_FLUTTER_TOOLCHAIN_PATH="$TARGET_ABS"
export VOIDPLAYER_FLUTTER_BIN="$TARGET_ABS/bin/flutter"

"$PYTHON_BIN" dev.py toolchain bootstrap-flutter
"$PYTHON_BIN" dev.py toolchain doctor

if [[ -n "${GITHUB_PATH:-}" ]]; then
  echo "$TARGET_ABS/bin" >> "$GITHUB_PATH"
fi

if [[ -n "${GITHUB_ENV:-}" ]]; then
  echo "VOIDPLAYER_FLUTTER_BIN=$VOIDPLAYER_FLUTTER_BIN" >> "$GITHUB_ENV"
fi
