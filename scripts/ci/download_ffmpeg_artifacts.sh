#!/usr/bin/env bash
set -euo pipefail

artifact_dir="${1:-build/ci-ffmpeg}"
shift || true
platforms=("$@")
if [[ ${#platforms[@]} -eq 0 ]]; then
  platforms=("windows-x64" "macos-arm64")
fi

repo="$(python3 scripts/ci/ffmpeg_lock.py repository)"
mkdir -p "$artifact_dir"

for platform in "${platforms[@]}"; do
  artifact_id="$(python3 scripts/ci/ffmpeg_lock.py artifact-id "$platform")"
  artifact_name="$(python3 scripts/ci/ffmpeg_lock.py artifact-name "$platform")"
  archive="$artifact_dir/${artifact_name}.github-artifact.zip"
  rm -f "$archive"
  gh api "repos/${repo}/actions/artifacts/${artifact_id}/zip" > "$archive"
  python3 scripts/ci/ffmpeg_lock.py verify-github-artifact "$platform" "$archive"
  unzip -q -o "$archive" -d "$artifact_dir"
done
