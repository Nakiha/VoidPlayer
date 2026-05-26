#!/usr/bin/env bash
set -euo pipefail

VOIDPLAYER_DISABLE_VIDEOTOOLBOX=1 python3.12 dev.py test --native-only --github

native/build-macos-make/videotoolbox_provider_smoke
native/build-macos-make/macos_native_player_shared_renderer_smoke
