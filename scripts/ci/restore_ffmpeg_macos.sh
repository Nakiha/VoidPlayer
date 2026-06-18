#!/usr/bin/env bash
set -euo pipefail

artifact_dir="${1:-build/ci-ffmpeg}"
install_dir="${VOIDPLAYER_FFMPEG_INSTALL_DIR:-$(python3 scripts/ci/ffmpeg_lock.py install-path macos-arm64)}"
artifact_name="$(python3 scripts/ci/ffmpeg_lock.py artifact-name macos-arm64)"
zip_path="$(find "$artifact_dir" -name "${artifact_name}.zip" -print -quit)"

if [[ -z "$zip_path" ]]; then
  echo "macOS FFmpeg artifact zip not found under $artifact_dir" >&2
  exit 1
fi

python3 scripts/ci/ffmpeg_lock.py verify macos-arm64 "$zip_path"
rm -rf "$artifact_dir/unpacked"
unzip -q "$zip_path" -d "$artifact_dir/unpacked"
rm -rf "$install_dir"
mkdir -p "$(dirname "$install_dir")"
mv "$artifact_dir/unpacked/$artifact_name" "$install_dir"

python3 scripts/ci/ffmpeg_lock.py build-notes macos-arm64 > "$install_dir/VOIDPLAYER_BUILD.md"

dylib="$(find "$install_dir/lib" -maxdepth 1 -type f -name 'libavcodec.*.dylib' | sort | tail -n 1)"
if [[ ! -s "$dylib" ]]; then
  echo "macOS FFmpeg dylib is missing or empty: $dylib" >&2
  exit 1
fi

if grep -q "version https://git-lfs.github.com/spec/v1" "$dylib"; then
  echo "macOS FFmpeg dylib is still a Git LFS pointer: $dylib" >&2
  exit 1
fi
