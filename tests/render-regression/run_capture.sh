#!/usr/bin/env bash
#============================================================================
# Copyright (C) 2013 - 2018, OpenJK contributors
#
# This file is part of the OpenJK source code.
#
# OpenJK is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.
#============================================================================

# Wraps capture.py with the environment each renderer actually needs to run
# headlessly, so a CMake target (see ../../CMakeLists.txt's
# add_subdirectory(tests/render-regression)) - or a person invoking this
# script by hand - never has to re-derive it. See code/rd-vulkan/README.md's
# "Testing headlessly" section for exactly why rd-vulkan's requirements
# (a real, virtual X server + Mesa's lavapipe software Vulkan driver) differ
# from rd-vanilla/rdsp-vanilla's (SDL2's "offscreen" driver, no display of
# any kind needed) - this script is that section's logic turned into
# something that can't be typo'd or forgotten.
set -euo pipefail

if [ "$#" -lt 5 ]; then
	echo "usage: $0 <renderer> <bindir> <fallback-basepath> <homepath> <outdir> [scene-filter]" >&2
	exit 2
fi

RENDERER="$1"
BINDIR="$2"
FALLBACK_BASEPATH="$3"
HOMEPATH="$4"
OUTDIR="$5"
FILTER="${6:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# RENDER_REGRESSION_BASEPATH (an environment variable, read fresh every time
# this script runs) always wins over the CMake-cache-configured fallback
# (RenderRegressionBasepath, baked in at configure time via -D) - switching
# which game install to test against should never require a reconfigure.
BASEPATH="${RENDER_REGRESSION_BASEPATH:-$FALLBACK_BASEPATH}"

if [ -z "$BASEPATH" ]; then
	cat >&2 <<'EOF'
error: no fs_basepath configured for the render-regression capture.

Point it at a directory containing your own legally-owned base/assets0-3.pk3
either way:
  - per-invocation (no reconfigure needed): export RENDER_REGRESSION_BASEPATH=/path/to/gamedata
  - persistently, at CMake configure time: -DRenderRegressionBasepath=/path/to/gamedata
EOF
	exit 1
fi

if [ ! -f "$BASEPATH/base/assets0.pk3" ]; then
	echo "error: '$BASEPATH/base/assets0.pk3' not found - '$BASEPATH' doesn't look like a real fs_basepath." >&2
	exit 1
fi

# A fresh homepath every run, deliberately: this project has already been
# bitten once by a stale saved cl_renderer (CVAR_ARCHIVE) in a reused
# homepath silently overriding capture.py's own --renderer flag - see
# capture.py's --renderer help text and code/rd-vulkan/README.md's "A
# capture-harness bug that invalidated a run of 'verified' claims" section
# for the full story. Wiping it every time removes that whole failure mode
# structurally instead of relying on remembering to pass --renderer
# correctly (which capture.py's own CLI already forces - this is defense in
# depth on top of that, not a replacement for it).
rm -rf "$HOMEPATH"
mkdir -p "$HOMEPATH" "$OUTDIR"

CAPTURE_ARGS=(--bindir "$BINDIR" --basepath "$BASEPATH" --homepath "$HOMEPATH" --out "$OUTDIR" --renderer "$RENDERER")
if [ -n "$FILTER" ]; then
	CAPTURE_ARGS+=(--filter "$FILTER")
fi

case "$RENDERER" in
	rd-vulkan)
		export SDL_VIDEODRIVER=x11
		if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
			# Mesa's lavapipe software Vulkan driver - the Vulkan equivalent
			# of llvmpipe, used the same way for headless testing. Only set
			# if the caller hasn't already picked a specific ICD themselves.
			export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
		fi
		if ! command -v xvfb-run >/dev/null 2>&1; then
			echo "error: xvfb-run not found - rd-vulkan needs a real (virtual) X server, see code/rd-vulkan/README.md's 'Testing headlessly' section." >&2
			exit 1
		fi
		exec xvfb-run -a --server-args="-screen 0 800x600x24" \
			python3 "$SCRIPT_DIR/capture.py" "${CAPTURE_ARGS[@]}"
		;;
	*)
		# rd-vanilla/rdsp-vanilla (or any other GL-based renderer): SDL2's
		# offscreen driver + software GL is enough, no display of any kind.
		export SDL_VIDEODRIVER=offscreen
		exec python3 "$SCRIPT_DIR/capture.py" "${CAPTURE_ARGS[@]}"
		;;
esac
