#!/usr/bin/env bash
set -euo pipefail

# Hosted macOS runners can expose Metal but still fail visible front-buffer
# capture in headless contexts. Keep the PR fast gate on the software/native
# suite and let CTest labels exclude hosted-flaky Metal canaries.
# Keep VideoToolbox canaries outside the software pass: the shared-renderer
# smoke intentionally exercises a VT-backed add/remove path, and the software
# env override can leave the later refresh subcase waiting on stale software
# preview frames.
VOIDPLAYER_DISABLE_VIDEOTOOLBOX=1 python3.12 dev.py test --native-only --github

ctest --test-dir native/build-macos-make --output-on-failure \
  -R "videotoolbox_provider_smoke|macos_native_player_shared_renderer_smoke"
