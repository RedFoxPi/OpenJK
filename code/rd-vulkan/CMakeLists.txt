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

if(BuildSPRdVulkan)

	find_package(Vulkan)
	if(NOT Vulkan_FOUND)
		message(WARNING "Vulkan SDK not found - skipping rd-vulkan (set BuildSPRdVulkan=OFF to silence this)")
		set(BuildSPRdVulkan OFF PARENT_SCOPE)
		return()
	endif()

	find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
	if(NOT GLSLANG_VALIDATOR)
		message(WARNING "glslangValidator not found - skipping rd-vulkan (install glslang-tools, or set BuildSPRdVulkan=OFF)")
		set(BuildSPRdVulkan OFF PARENT_SCOPE)
		return()
	endif()

	set(SPRDVulkanRendererIncludeDirectories
		${SPDir}
		${SharedDir}
		"${SPDir}/rd-vulkan"
		"${SPDir}/rd-common"
		)

	set(SPRDVulkanDefines ${SharedDefines} "RENDERER" "_JK2EXE")

	# GHOUL2 (G2_*.cpp / tr_ghoul2.cpp) is NOT reused here, despite having no
	# direct GL calls itself: those files use #include "tr_local.h" as a
	# quote-include, which resolves against their OWN directory
	# (code/rd-vanilla/) before any -I search path, so compiling them here
	# silently pulls in rd-vanilla's real GL-coupled trGlobals_t/model_t/
	# shader_t types (and needs rd-vanilla's model/shader/skin registries -
	# R_GetModelByHandle et al - to actually link). Real reuse needs rd-
	# vanilla's asset-parsing layer ported over too, which is future work -
	# see rd-vulkan/README.md. G2API_* is stubbed in tr_init.cpp, except for
	# model loading/rendering itself (tr_model.cpp), which parses .glm files
	# directly using the shared, GL-agnostic mdx_format.h structs.

	list(APPEND SPRDVulkanRendererIncludeDirectories ${JPEG_INCLUDE_DIR})
	list(APPEND SPRDVulkanRendererLibraries          ${JPEG_LIBRARIES})
	list(APPEND SPRDVulkanRendererIncludeDirectories ${PNG_INCLUDE_DIRS})
	list(APPEND SPRDVulkanRendererLibraries          ${PNG_LIBRARIES})
	list(APPEND SPRDVulkanRendererIncludeDirectories ${ZLIB_INCLUDE_DIR})
	list(APPEND SPRDVulkanRendererLibraries          ${ZLIB_LIBRARIES})
	list(APPEND SPRDVulkanRendererIncludeDirectories ${MINIZIP_INCLUDE_DIRS})
	list(APPEND SPRDVulkanRendererLibraries          ${MINIZIP_LIBRARIES})

	set(SPRDVulkanCommonFiles
		"${SPDir}/qcommon/matcomp.cpp"
		"${SPDir}/qcommon/q_shared.cpp"
		"${SPDir}/qcommon/q_shared.h"
		"${SPDir}/qcommon/ojk_i_saved_game.h"
		"${SPDir}/qcommon/ojk_saved_game_class_archivers.h"
		"${SPDir}/qcommon/ojk_saved_game_helper.h"
		"${SPDir}/qcommon/ojk_saved_game_helper_fwd.h"
		"${SPDir}/qcommon/ojk_scope_guard.h"
		${SharedCommonFiles}
		)
	source_group("common" FILES ${SPRDVulkanCommonFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanCommonFiles})

	set(SPRDVulkanCommonSafeFiles ${SharedCommonSafeFiles})
	source_group("common/safe" FILES ${SPRDVulkanCommonSafeFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanCommonSafeFiles})

	# rd-common - reused verbatim: image decode (libpng/jpeg/tga) and font
	# glyph-layout math are graphics-API-agnostic. See rd-vulkan/README.md.
	set(SPRDVulkanRdCommonFiles
		"${SPDir}/rd-common/mdx_format.h"
		"${SPDir}/rd-common/tr_common.h"
		"${SPDir}/rd-common/tr_font.cpp"
		"${SPDir}/rd-common/tr_font.h"
		"${SPDir}/rd-common/tr_image_load.cpp"
		"${SPDir}/rd-common/tr_image_jpg.cpp"
		"${SPDir}/rd-common/tr_image_tga.cpp"
		"${SPDir}/rd-common/tr_image_png.cpp"
		"${SPDir}/rd-common/tr_noise.cpp"
		"${SPDir}/rd-common/tr_public.h"
		"${SPDir}/rd-common/tr_types.h")
	source_group("rd-common" FILES ${SPRDVulkanRdCommonFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanRdCommonFiles})

	# main files - actually Vulkan-specific
	set(SPRDVulkanMainFiles
		"${SPDir}/rd-vulkan/tr_local.h"
		"${SPDir}/rd-vulkan/tr_init.cpp"
		"${SPDir}/rd-vulkan/tr_image.cpp"
		"${SPDir}/rd-vulkan/tr_cmds.cpp"
		"${SPDir}/rd-vulkan/tr_shader.cpp"
		"${SPDir}/rd-vulkan/tr_world.cpp"
		"${SPDir}/rd-vulkan/tr_model.cpp"
		"${SPDir}/rd-vulkan/tr_weather.cpp"
		)
	source_group("renderer" FILES ${SPRDVulkanMainFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanMainFiles})

	list(APPEND SPRDVulkanRendererIncludeDirectories ${OpenJKLibDir})
	list(APPEND SPRDVulkanRendererLibraries Vulkan::Vulkan)

	# Needed directly (rather than inherited from the parent scope) because
	# this subdirectory is added before code/CMakeLists.txt runs its own
	# find_package(SDL2) - SDL_vulkan.h (SDL_Vulkan_CreateSurface() etc.) is
	# part of the same SDL2 headers, no extra package required.
	find_package(SDL2 CONFIG)
	if(TARGET SDL2::SDL2)
		list(APPEND SPRDVulkanRendererLibraries SDL2::SDL2)
	else()
		find_package(SDL2 MODULE REQUIRED)
		list(APPEND SPRDVulkanRendererIncludeDirectories ${SDL2_INCLUDE_DIRS})
		list(APPEND SPRDVulkanRendererLibraries ${SDL2_LIBRARIES})
	endif()

	# Compile the UI shaders to SPIR-V and embed them as C headers, so the
	# renderer is a single self-contained .so with nothing extra to ship.
	set(SPRDVulkanShaderDir "${SPDir}/rd-vulkan/shaders")
	set(SPRDVulkanShaderGenDir "${CMAKE_CURRENT_BINARY_DIR}/rd-vulkan-shaders")
	file(MAKE_DIRECTORY ${SPRDVulkanShaderGenDir})

	function(compile_and_embed_shader SrcName VarName)
		set(spv "${SPRDVulkanShaderGenDir}/${SrcName}.spv")
		set(hdr "${SPRDVulkanShaderGenDir}/${VarName}.h")
		add_custom_command(
			OUTPUT ${hdr}
			COMMAND ${GLSLANG_VALIDATOR} -V "${SPRDVulkanShaderDir}/${SrcName}" -o ${spv}
			COMMAND ${CMAKE_COMMAND} -DINPUT_FILE=${spv} -DOUTPUT_FILE=${hdr} -DVAR_NAME=${VarName}
				-P "${SPDir}/rd-vulkan/bin2header.cmake"
			DEPENDS "${SPRDVulkanShaderDir}/${SrcName}" "${SPDir}/rd-vulkan/bin2header.cmake"
			COMMENT "Compiling and embedding shader ${SrcName}"
			VERBATIM
			)
		set(SPRDVulkanGeneratedHeaders ${SPRDVulkanGeneratedHeaders} ${hdr} PARENT_SCOPE)
	endfunction()

	compile_and_embed_shader("ui.vert" "ui_vert_spv")
	compile_and_embed_shader("ui.frag" "ui_frag_spv")
	compile_and_embed_shader("world.vert" "world_vert_spv")
	compile_and_embed_shader("world.frag" "world_frag_spv")
	compile_and_embed_shader("poly.vert" "poly_vert_spv")
	compile_and_embed_shader("poly.frag" "poly_frag_spv")

	list(APPEND SPRDVulkanRendererIncludeDirectories ${SPRDVulkanShaderGenDir})

	function(add_sp_vulkan_renderer_project ProjectName Label EngineName InstallDir Component)
		add_library(${ProjectName} SHARED ${SPRDVulkanFiles} ${SPRDVulkanGeneratedHeaders})
		if(NOT MSVC)
			set_target_properties(${ProjectName} PROPERTIES PREFIX "")
		endif()

		if(WIN32)
			install(TARGETS ${ProjectName}
				RUNTIME
				DESTINATION ${InstallDir}
				COMPONENT ${Component})
		else(WIN32)
			if(MakeApplicationBundles AND (BuildSPEngine OR BuildJK2SPEngine))
				install(TARGETS ${ProjectName}
					LIBRARY
					DESTINATION "${InstallDir}/${EngineName}.app/Contents/MacOS/"
					COMPONENT ${Component})
			else()
				install(TARGETS ${ProjectName}
					LIBRARY
					DESTINATION ${InstallDir}
					COMPONENT ${Component})
			endif()
		endif()

		set_target_properties(${ProjectName} PROPERTIES COMPILE_DEFINITIONS "${SPRDVulkanDefines}")
		set_property(TARGET ${ProjectName} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})
		set_target_properties(${ProjectName} PROPERTIES INCLUDE_DIRECTORIES "${SPRDVulkanRendererIncludeDirectories}")
		set_target_properties(${ProjectName} PROPERTIES PROJECT_LABEL ${Label})

		target_link_libraries(${ProjectName} ${SPRDVulkanRendererLibraries})
	endfunction(add_sp_vulkan_renderer_project)

	add_sp_vulkan_renderer_project(${SPRDVulkanRenderer} "SP Vulkan Renderer" ${SPEngine} ${JKAInstallDir} ${JKASPClientComponent})

endif(BuildSPRdVulkan)
