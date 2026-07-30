#!/usr/bin/env bash
# build-splash-gif.sh — regenerates the indexed animated about:start splash.
set -euo pipefail

cd "$(dirname "$0")/.."
python3 scripts/build-splash-gif.py
