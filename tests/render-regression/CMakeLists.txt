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

# Make sure the user is not executing this script directly
if(NOT InOpenJK)
	message(FATAL_ERROR "Use the top-level cmake script!")
endif(NOT InOpenJK)

# Real CMake targets for the headless render-regression capture harness
# (README.md in this directory), wired up so "which .so did this actually
# just copy/exercise" can never again be a guess. Every binary staged below
# is named via $<TARGET_FILE:...> - CMake's own generator expression for
# "wherever this target's build actually produced its output" - never a
# hand-typed path that can quietly point at a stale leftover file from an
# earlier build layout. This exists because that exact mistake happened
# during development: rd-vulkan's actual build output lives at
# code/rd-vulkan/<name>.so (a target-owning-subdirectory CMake convention),
# but a stale copy of an old build had been sitting directly under the
# top-level build directory from an earlier point in the project - manual
# `cp build-vulkan/rd-vulkan_x86_64.so ...` commands kept silently picking
# up that stale file instead of the freshly-rebuilt one for an entire
# session's worth of "verification" before the mixup was caught (see
# code/rd-vulkan/README.md's "A capture-harness bug that invalidated a run
# of 'verified' claims" section for the precedent this follows, and the
# fix's own commit for this specific incident). A generator expression
# cannot be wrong about this the way a person guessing a path can.
#
# Targets defined here (only for renderers actually enabled/available):
#   stage_render_regression_sp - internal, copies engine+renderer+gamecode
#                                binaries into a staging "bindir"
#   render_regression_vulkan   - runs the full SP scene suite against rd-vulkan
#   render_regression_vanilla  - runs it against rdsp-vanilla
#   render_regression_diff     - runs both of the above, then diffs the two
#   render_regression          - alias for render_regression_diff
#
# None of these are part of the default `ninja`/`make` build (no ALL) -
# they're opt-in, e.g. `ninja render_regression_vulkan`.
#
# fs_basepath (a directory containing your own legally-owned
# base/assets0-3.pk3) is never known to this repo and never committed - see
# this directory's own README.md. Configure it once via
# -DRenderRegressionBasepath=/path/to/gamedata, or override it per-
# invocation (no reconfigure needed) via the RENDER_REGRESSION_BASEPATH
# environment variable - run_capture.sh checks the environment variable
# first, see its own comment for why.
if(NOT BuildSPEngine)
	return()
endif()

set(RenderRegressionBasepath "" CACHE PATH
	"fs_basepath for the render-regression targets below (a directory containing base/assets0-3.pk3) - overridable per-run via the RENDER_REGRESSION_BASEPATH environment variable, see run_capture.sh")

set(RenderRegressionRoot   "${CMAKE_BINARY_DIR}/render-regression")
set(RenderRegressionStage  "${RenderRegressionRoot}/JediAcademy")

set(RenderRegressionStageTargets "")
set(RenderRegressionStageCommands "")

# Only stages a binary if its target actually exists (e.g. BuildSPRdVulkan
# can be OFF if the Vulkan SDK/glslangValidator weren't found - see
# code/rd-vulkan/CMakeLists.txt) - silently skipping a missing one here
# rather than failing the configure, since which renderers are even
# buildable in a given environment is exactly the kind of thing this
# subdirectory shouldn't need to know about ahead of time.
function(render_regression_stage_binary target_name)
	if(TARGET ${target_name})
		set(RenderRegressionStageTargets ${RenderRegressionStageTargets} ${target_name} PARENT_SCOPE)
		set(RenderRegressionStageCommands ${RenderRegressionStageCommands}
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target_name}>" "${RenderRegressionStage}/"
			PARENT_SCOPE)
	endif()
endfunction()

render_regression_stage_binary(${SPEngine})
render_regression_stage_binary(${SPGame})
render_regression_stage_binary(${SPRDVanillaRenderer})
render_regression_stage_binary(${SPRDVulkanRenderer})

if(NOT RenderRegressionStageTargets)
	return()
endif()

add_custom_target(stage_render_regression_sp
	COMMAND ${CMAKE_COMMAND} -E make_directory "${RenderRegressionStage}"
	${RenderRegressionStageCommands}
	DEPENDS ${RenderRegressionStageTargets}
	COMMENT "render-regression: staging SP engine + renderer binaries (via each target's own real build output path - never a hand-typed one)"
	VERBATIM
	)

# Defines <name_prefix> as a target that stages binaries then runs
# run_capture.sh against <renderer_target>/<renderer_flag>, writing into its
# own homepath/output directory under RenderRegressionRoot (one per
# renderer, so a vulkan and a vanilla run never clobber each other's
# results - render_regression_diff below needs both to still exist at once).
# Filtered to scenes.json's "sp_" scenes only, matching what's actually
# staged above (no MP engine/gamecode/cgame/ui) - the mp_* scenes need
# BuildMPEngine (and friends) ON and are out of scope for these SP-focused
# targets; run capture.py directly (see this directory's own README.md) for
# MP coverage.
function(add_render_regression_capture_target name_prefix renderer_target renderer_flag)
	if(NOT TARGET ${renderer_target})
		return()
	endif()
	add_custom_target(${name_prefix}
		COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_capture.sh"
			"${renderer_flag}"
			"${RenderRegressionStage}"
			"${RenderRegressionBasepath}"
			"${RenderRegressionRoot}/home-${renderer_flag}"
			"${RenderRegressionRoot}/screenshots-${renderer_flag}"
			"sp_"
		DEPENDS stage_render_regression_sp
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "render-regression: running SP scene suite against ${renderer_flag}"
		USES_TERMINAL
		VERBATIM
		)
endfunction()

add_render_regression_capture_target(render_regression_vulkan  ${SPRDVulkanRenderer}  "rd-vulkan")
add_render_regression_capture_target(render_regression_vanilla ${SPRDVanillaRenderer} "rdsp-vanilla")

if(TARGET render_regression_vulkan AND TARGET render_regression_vanilla)
	# diff.py exits nonzero whenever it finds a MAJOR_DIFF/MINOR_DIFF scene
	# (not just on a real script error), so ninja/make will report this
	# target as "FAILED" any time the two renderers' output differs
	# meaningfully - which, per this project's own documented baseline (see
	# code/rd-vulkan/README.md), is still expected/normal at this stage of
	# rd-vulkan's development (a real GL-vs-Vulkan renderer rewrite is not
	# going to be pixel-identical to the reference). Read the actual
	# per-scene summary this target prints (and the .diff.png images it
	# writes) rather than treating "ninja says FAILED" alone as a
	# regression signal.
	add_custom_target(render_regression_diff
		COMMAND ${CMAKE_COMMAND} -E make_directory "${RenderRegressionRoot}/diff"
		COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/diff.py"
			--a "${RenderRegressionRoot}/screenshots-rdsp-vanilla"
			--b "${RenderRegressionRoot}/screenshots-rd-vulkan"
			--out "${RenderRegressionRoot}/diff"
		DEPENDS render_regression_vulkan render_regression_vanilla
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "render-regression: diffing rdsp-vanilla vs rd-vulkan screenshots (requires Pillow - pip install pillow)"
		USES_TERMINAL
		VERBATIM
		)
	add_custom_target(render_regression DEPENDS render_regression_diff)
endif()
