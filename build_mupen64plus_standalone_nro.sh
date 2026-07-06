#!/bin/bash
# Standalone build (no libretro frame pump) — see STANDALONE_PLAN.md.
# The emulator free-runs on a dedicated pthread (core 1) and presents from the
# N64 VI path; FIFO vsync paces emulation. Output: build_tico_standalone/mupen64plus.nro
exec env TICO_STANDALONE=1 "$(cd "$(dirname "$0")" && pwd)/build_mupen64plus_nro.sh" "$@"
