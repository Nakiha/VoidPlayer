#!/usr/bin/env bash
set -euo pipefail

# Hosted macOS runners can expose Metal but still fail visible front-buffer
# capture in headless contexts. Keep the PR fast gate on the software/native
# suite and let CTest labels exclude hosted-flaky Metal canaries.
VOIDPLAYER_DISABLE_VIDEOTOOLBOX=1 python3.12 dev.py test --native-only --github
