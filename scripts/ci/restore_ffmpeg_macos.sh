#!/usr/bin/env bash
set -euo pipefail

artifact_dir="${1:-build/ci-ffmpeg}"
run_id="${VOIDPLAYER_FFMPEG_BUILD_RUN_ID:-}"
artifact_name="voidplayer-ffmpeg-macos-arm64-n8.1"
zip_path="$(find "$artifact_dir" -name "${artifact_name}.zip" -print -quit)"

if [[ -z "$zip_path" ]]; then
  echo "macOS FFmpeg artifact zip not found under $artifact_dir" >&2
  exit 1
fi

rm -rf "$artifact_dir/unpacked"
unzip -q "$zip_path" -d "$artifact_dir/unpacked"
rm -rf third_party/ffmpeg
mkdir -p third_party
mv "$artifact_dir/unpacked/$artifact_name" third_party/ffmpeg

{
  echo "# VoidPlayer FFmpeg Package"
  echo
  echo "Restored in CI from the latest successful VoidPlayer FFmpeg build."
  echo
  echo "- Repository: https://github.com/Nakiha/VoidPlayer-FFmpeg-Build"
  if [[ -n "$run_id" ]]; then
    echo "- GitHub Actions run: https://github.com/Nakiha/VoidPlayer-FFmpeg-Build/actions/runs/$run_id"
  fi
  echo "- Artifact: $artifact_name"
} > third_party/ffmpeg/VOIDPLAYER_BUILD.md

dylib="third_party/ffmpeg/lib/libavcodec.62.28.100.dylib"
if [[ ! -s "$dylib" ]]; then
  echo "macOS FFmpeg dylib is missing or empty: $dylib" >&2
  exit 1
fi

if grep -q "version https://git-lfs.github.com/spec/v1" "$dylib"; then
  echo "macOS FFmpeg dylib is still a Git LFS pointer: $dylib" >&2
  exit 1
fi
