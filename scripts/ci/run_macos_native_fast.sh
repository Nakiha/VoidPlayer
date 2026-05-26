#!/usr/bin/env bash
set -euo pipefail

VOIDPLAYER_DISABLE_VIDEOTOOLBOX=1 python3.12 dev.py test --native-only --github
