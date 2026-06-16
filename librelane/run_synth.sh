#!/usr/bin/env bash
# Run LibreLane's Classic flow, stopping after Yosys.Synthesis. Produces
# a synthesized gate-level netlist (z80_core.nl.v) under
# librelane/runs/<timestamp>/final/nl/.
#
# Side effects (consumed by the Makefile):
#   build/synth/z80_core.nl.v       — symlink to the latest run's netlist
#   build/synth/pdk_root.path       — single-line file with the resolved
#                                     PDK_ROOT path, so `make iverilog_netlist`
#                                     can find the sky130_fd_sc_hd Verilog
#                                     cell models without guessing.
#
# Invoked by `make synth` from the repo root. Expects `librelane` on
# PATH (Nix profile install — see docs/librelane-flow.md "Tooling").
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/librelane"

if ! command -v librelane >/dev/null 2>&1; then
  cat >&2 <<'EOF'
librelane not found on PATH.

Install with Nix (the project's recommended native install path — see
docs/librelane-flow.md "Tooling"). The fossi-foundation substituter
MUST be configured before installing or nix will try to rebuild
LibreLane's pinned iverilog snapshot from source, which fails on
x86_64-linux (1 of 297 self-tests fails).

  curl --proto '=https' --tlsv1.2 -fsSL https://install.determinate.systems/nix \
    | sh -s -- install --no-confirm --extra-conf "
        extra-substituters = https://nix-cache.fossi-foundation.org
        extra-trusted-public-keys = nix-cache.fossi-foundation.org:3+K59iFwXqKsL7BNu6Guy0v+uTlwsxYQxjspXzqLYQs=
        extra-experimental-features = nix-command flakes
      "
  nix profile install github:librelane/librelane

Then re-run `make synth`.
EOF
  exit 2
fi

# --to Yosys.Synthesis runs only up through the synthesis step; no
# floorplan / place / route / CTS / STA — those add minutes per push
# and produce no signals our gate-level sim needs.
librelane --to Yosys.Synthesis ./config.json

# Locate the latest run's netlist + the PDK_ROOT it was synthesized
# against. Both go into build/synth/ as a stable interface for the
# Makefile rules downstream.
mkdir -p "$ROOT/build/synth"

latest=$(ls -dt runs/*/final/nl/z80_core.nl.v 2>/dev/null | head -1 || true)
if [ -z "$latest" ]; then
  echo "synth: no netlist produced — check librelane/runs/*/" >&2
  exit 1
fi
ln -sf "$ROOT/librelane/$latest" "$ROOT/build/synth/z80_core.nl.v"
echo "synth: produced $latest"

# Resolve PDK_ROOT — try in order:
#   1. Environment (if LibreLane was invoked with PDK_ROOT preset)
#   2. The resolved-config JSON from the latest run (LibreLane stamps
#      its effective PDK_ROOT here)
#   3. Default volare location ~/.volare/sky130A — Volare is the PDK
#      manager LibreLane uses; this is its standard install path.
latest_run=$(ls -dt runs/*/ | head -1)
pdk_root=""
if [ -n "${PDK_ROOT:-}" ]; then
  pdk_root="$PDK_ROOT"
fi
if [ -z "$pdk_root" ] && [ -f "$latest_run/resolved.json" ]; then
  pdk_root=$(python3 -c "
import json, sys
with open('$latest_run/resolved.json') as f:
    d = json.load(f)
print(d.get('PDK_ROOT', ''))
" 2>/dev/null || true)
fi
if [ -z "$pdk_root" ] && [ -d "$HOME/.volare/sky130A" ]; then
  pdk_root="$HOME/.volare"
fi

if [ -z "$pdk_root" ] || [ ! -d "$pdk_root/sky130A" ]; then
  echo "synth: could not resolve PDK_ROOT (looked in env, resolved.json, ~/.volare)" >&2
  echo "  set PDK_ROOT manually or check that volare installed sky130A" >&2
  exit 1
fi
echo "$pdk_root" > "$ROOT/build/synth/pdk_root.path"
echo "synth: PDK_ROOT=$pdk_root"
