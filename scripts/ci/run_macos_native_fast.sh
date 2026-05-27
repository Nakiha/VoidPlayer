#!/usr/bin/env bash
set -euo pipefail

# Hosted macOS runners can expose Metal but still fail visible front-buffer
# capture in headless contexts. Keep the PR fast gate on the software/native
# suite and let CTest labels exclude hosted-flaky Metal canaries.
# Keep VideoToolbox provider probing outside the software pass. The full
# shared-renderer VT smoke is intentionally local/nightly: hosted macOS runners
# can report VT availability but fail real H.264 VT frame output.
VOIDPLAYER_DISABLE_VIDEOTOOLBOX=1 python3.12 dev.py test --native-only --github

ctest --test-dir native/build-macos-make --output-on-failure \
  -R "videotoolbox_provider_smoke"
