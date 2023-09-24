cmake_minimum_required(VERSION 3.21)

project(
	CommunityShaders
	VERSION 0.6.6
	LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ########################################################################################################################
# ## Build options
# ########################################################################################################################
message("Options:")
option(AUTO_PLUGIN_DEPLOYMENT "Copy the build output and addons to env:CommunityShadersOutputDir." OFF)
option(ZIP_TO_DIST "Zip the base mod and addons to their own 7z file in dist." ON)
message("\tAuto plugin deployment: ${AUTO_PLUGIN_DEPLOYMENT}")
message("\tZip to dist: ${ZIP_TO_DIST}")

# #######################################################################################################################
# # Add CMake features
# #######################################################################################################################
include(XSEPlugin)

# #######################################################################################################################
# # Find dependencies
# #######################################################################################################################
find_path(BSHOSHANY_THREAD_POOL_INCLUDE_DIRS "BS_thread_pool.hpp")
find_package(magic_enum CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(EASTL CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")
find_package(pystring CONFIG REQUIRED)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	${BSHOSHANY_THREAD_POOL_INCLUDE_DIRS}
	${CLIB_UTIL_INCLUDE_DIRS}
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	debug ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Debug/detours.lib
	optimized ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Release/detours.lib
	magic_enum::magic_enum
	xbyak::xbyak
	nlohmann_json::nlohmann_json
	imgui::imgui
	EASTL
	Microsoft::DirectXTK
	pystring::pystring
)

# https://gitlab.kitware.com/cmake/cmake/-/issues/24922#note_1371990
if(MSVC_VERSION GREATER_EQUAL 1936 AND MSVC_IDE) # 17.6+
	# When using /std:c++latest, "Build ISO C++23 Standard Library Modules" defaults to "Yes".
	# Default to "No" instead.
	#
	# As of CMake 3.26.4, there isn't a way to control this property
	# (https://gitlab.kitware.com/cmake/cmake/-/issues/24922),
	# We'll use the MSBuild project system instead
	# (https://learn.microsoft.com/en-us/cpp/build/reference/vcxproj-file-structure)
	file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/Directory.Build.props" CONTENT [==[
<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <BuildStlModules>false</BuildStlModules>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
]==] @ONLY)
endif()

# #######################################################################################################################
# # Automatic deployment
# #######################################################################################################################

file(GLOB FEATURE_PATHS LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/features/*)

# Automatic deployment to CommunityShaders output directory.
if (AUTO_PLUGIN_DEPLOYMENT)
	foreach(DEPLOY_TARGET $ENV{CommunityShadersOutputDir})
		message("Copying package folder with dll/pdb with all features to ${DEPLOY_TARGET}")
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/package "${DEPLOY_TARGET}"
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${DEPLOY_TARGET}/SKSE/Plugins/"
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${DEPLOY_TARGET}/SKSE/Plugins/"
		)
		foreach(FEATURE_PATH ${FEATURE_PATHS})
			add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_directory ${FEATURE_PATH} "${DEPLOY_TARGET}"
			)
		endforeach()
	endforeach()
	if (NOT DEFINED ENV{CommunityShadersOutputDir})
		message("When using AUTO_PLUGIN_DEPLOYMENT option, you need to set environment variable 'CommunityShadersOutputDir'")
	endif()
endif()

# Zip base CommunityShaders and all addons as their own 7z in dist folder
if(ZIP_TO_DIST)
	set(ZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/zip")
	file(REMOVE_RECURSE ${CMAKE_BINARY_DIR}/test/)
	add_custom_target(build-time-make-directory ALL
		COMMAND ${CMAKE_COMMAND} -E remove_directory "${ZIP_DIR}" ${CMAKE_SOURCE_DIR}/dist
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ZIP_DIR}" ${CMAKE_SOURCE_DIR}/dist
	)

	message("Copying base CommunityShader into ${ZIP_DIR}.")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/package "${ZIP_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ZIP_DIR}/SKSE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${ZIP_DIR}/SKSE/Plugins/"
	)

	set(TARGET_ZIP "${PROJECT_NAME}.7z")
	message("Zipping ${ZIP_DIR} to ${CMAKE_SOURCE_DIR}/dist/${TARGET_ZIP}")
	ADD_CUSTOM_COMMAND(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_SOURCE_DIR}/dist/${TARGET_ZIP} --format=7zip -- .
		WORKING_DIRECTORY ${ZIP_DIR}
	)

	foreach(FEATURE_PATH ${FEATURE_PATHS})
		get_filename_component(FEATURE ${FEATURE_PATH} NAME)
		message("Zipping ${FEATURE_PATH} to ${CMAKE_SOURCE_DIR}/dist/${FEATURE}.7z")
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_SOURCE_DIR}/dist/${FEATURE}.7z --format=7zip -- .
			WORKING_DIRECTORY ${FEATURE_PATH}
		)
	endforeach()
endif()
