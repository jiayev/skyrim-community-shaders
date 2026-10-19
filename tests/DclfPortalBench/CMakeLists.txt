# The portal culling bench: the production portal shaders (PortalBatchCS) and the experimental evaluators (PortalEvaluators.hlsli),
# run outside the game on a BasicRHI Vulkan device of their own, against real (.dclfportal captures) and synthetic portal graphs.
#
#   cmake -S tests/DclfPortalBench -B build/dclf-portal-bench -DCMAKE_PREFIX_PATH=<vcpkg_installed>/x64-windows-static-md-release
#   cmake --build build/dclf-portal-bench --config Release
#   build/dclf-portal-bench/Release/DclfPortalBench.exe --synthetic --csv portal-bench.csv [--corpus <file or directory>]...
#
# BasicRHI comes from the extern/ submodule; its packages (spdlog, fmt, volk) from the plugin's vcpkg tree (CMAKE_PREFIX_PATH, e.g.
# build/ALL/vcpkg_installed/x64-windows-static-md-release); DXC is the Vulkan SDK's.
cmake_minimum_required(VERSION 3.21)
project(DclfPortalBench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(_cs_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CS_ORG_ROOT "${_cs_root}/extern" CACHE PATH "Directory holding BasicRHI, BasicTelemetry, OpenRenderGraph and volk")

set(BASICRHI_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_STREAMLINE OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_PIX OFF CACHE BOOL "" FORCE)
set(BASICRHI_ENABLE_TRACY_GPU_PROFILING OFF CACHE BOOL "" FORCE)
set(BASICRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CS_ORG_ROOT}/BasicRHI" "${CMAKE_BINARY_DIR}/BasicRHI" EXCLUDE_FROM_ALL)
if(TARGET BasicRHI AND NOT TARGET BasicRHI::BasicRHI)
    add_library(BasicRHI::BasicRHI ALIAS BasicRHI)
endif()

include("${CS_ORG_ROOT}/BasicRHI/cmake/BasicRHIShaderFlags.cmake")

set(_shaders "${_cs_root}/features/Drawcall Limit Fix/Shaders")
set(_batch "${_shaders}/DrawcallLimitFix/PortalBatchCS.hlsl")
file(GLOB _portal_includes "${_shaders}/DrawcallLimitFix/Portal*.hlsli")
set(_spv_dir "${CMAKE_BINARY_DIR}/spirv")
set(_spirv)
# name entry defines...
function(portal_spirv name entry)
    set(_out "${_spv_dir}/${name}.spv")
    basicrhi_compile_spirv(OUTPUT "${_out}" SOURCE "${_batch}" ENTRY ${entry} PROFILE cs_6_6
        DEFINES ${ARGN} INCLUDE_DIRS "${_shaders}" DEPENDS ${_portal_includes})
    set(_spirv ${_spirv} "${_out}" PARENT_SCOPE)
endfunction()
foreach(_entry Clear Count TaskCounts Scan AddOffsets Scatter MakeTasks Evaluate)
    portal_spirv(batch-${_entry} ${_entry})
endforeach()
portal_spirv(batch-Evaluate-uniform Evaluate DCLF_PORTAL_UNIFORM_SCAN=1)
portal_spirv(batch-Evaluate-planebits Evaluate DCLF_PORTAL_EXPERIMENTAL=1 DCLF_PORTAL_PLANE_BITS=1)
portal_spirv(inline Inline)
portal_spirv(inline-floor Inline DCLF_PORTAL_FLOOR=1)
portal_spirv(inline-uniform Inline DCLF_PORTAL_UNIFORM_SCAN=1)
portal_spirv(inline-flat Inline DCLF_PORTAL_EXPERIMENTAL=1 DCLF_PORTAL_FLAT=1)
portal_spirv(inline-planebits Inline DCLF_PORTAL_EXPERIMENTAL=1 DCLF_PORTAL_PLANE_BITS=1)

add_executable(DclfPortalBench main.cpp Corpus.h Synthetic.h AuxTables.h Gpu.h ${_spirv})
target_include_directories(DclfPortalBench PRIVATE "${_cs_root}/src")
target_compile_definitions(DclfPortalBench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS DCLF_PORTAL_BENCH_SPIRV="${_spv_dir}")
if(MSVC)
    target_compile_options(DclfPortalBench PRIVATE /utf-8 /W4 /permissive-)
endif()
target_link_libraries(DclfPortalBench PRIVATE BasicRHI::BasicRHI)
