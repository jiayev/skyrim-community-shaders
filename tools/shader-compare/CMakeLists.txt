# dclf-shadertool: the game's DCLF shader source transforms, offline. Its own small build (shader_compare.py configures it in
# build/shader-tools), so the tool builds in seconds without the plugin.
cmake_minimum_required(VERSION 3.24)
project(dclf-shadertool CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(dclf-shadertool
	dclf-shadertool.cpp
	${CS_ROOT}/src/Features/DrawcallLimitFix/Core/Gpu/Pipelines/PulledSource.cpp)
target_include_directories(dclf-shadertool PRIVATE ${CS_ROOT}/src)
if(MSVC)
	target_compile_options(dclf-shadertool PRIVATE /utf-8 /W4)
endif()

# spv-pipeline-stats: the driver's statistics of a pipeline made from SPIR-V (needs the Vulkan SDK).
find_package(Vulkan REQUIRED)
add_executable(spv-pipeline-stats spv-pipeline-stats.cpp)
target_link_libraries(spv-pipeline-stats PRIVATE Vulkan::Vulkan)
target_include_directories(spv-pipeline-stats PRIVATE $ENV{VULKAN_SDK}/Include ${Vulkan_INCLUDE_DIRS})
if(MSVC)
	target_compile_options(spv-pipeline-stats PRIVATE /utf-8 /W4)
endif()

# spv-lower: BasicRHI's descriptor-buffer lowering offline (its SPIRV-Tools and SPIRV-Headers submodules, as BasicRHI builds them).
set(BASICRHI_ROOT ${CS_ROOT}/extern/BasicRHI)
set(SPIRV-Headers_SOURCE_DIR "${BASICRHI_ROOT}/third_party/SPIRV-Headers")
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${BASICRHI_ROOT}/third_party/SPIRV-Tools" "${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Tools" EXCLUDE_FROM_ALL)
add_executable(spv-lower spv-lower.cpp ${BASICRHI_ROOT}/rhi_vulkan_spirv_lowering.cpp)
target_include_directories(spv-lower PRIVATE ${BASICRHI_ROOT} ${CS_ROOT}/extern/volk "${BASICRHI_ROOT}/third_party/SPIRV-Tools"
	"${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Tools" $ENV{VULKAN_SDK}/Include)
target_compile_definitions(spv-lower PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(spv-lower PRIVATE SPIRV-Tools-opt)
if(MSVC)
	target_compile_options(spv-lower PRIVATE /utf-8 /bigobj)
endif()
