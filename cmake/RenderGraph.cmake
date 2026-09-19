# OpenRenderGraph on DXVK's Vulkan device (src/RenderGraph, Light Limit Fix light culling).
#
# BasicRHI adopts the VkDevice DXVK creates for the game, and OpenRenderGraph runs its
# passes on it in persistent execution. The libraries are built from source with the
# Vulkan backend only, from the extern/ submodules (BasicRHI, BasicTelemetry,
# OpenRenderGraph, volk). To build against local changes to those libraries, point
# CS_ORG_ROOT at another directory with the same side-by-side layout.
#
# Without them the plugin still builds: CS_HAS_RENDER_GRAPH stays off and every feature
# keeps its D3D11 path.

option(CS_RENDER_GRAPH "Build the OpenRenderGraph runtime that adopts DXVK's Vulkan device" ON)
set(CS_ORG_ROOT "${CMAKE_SOURCE_DIR}/extern" CACHE PATH
    "Directory holding BasicRHI, BasicTelemetry, OpenRenderGraph and volk side by side")

set(CS_HAS_RENDER_GRAPH OFF)
set(CS_GENERATED_SHADER_DIR "${CMAKE_BINARY_DIR}/generated/Shaders")
set(CS_RENDER_GRAPH_SPIRV)

if(CS_RENDER_GRAPH)
    set(_cs_org_missing)
    foreach(_lib IN ITEMS OpenRenderGraph BasicRHI BasicTelemetry volk)
        if(NOT EXISTS "${CS_ORG_ROOT}/${_lib}/CMakeLists.txt")
            list(APPEND _cs_org_missing "${_lib}")
        endif()
    endforeach()

    if(_cs_org_missing)
        message(WARNING "CS_RENDER_GRAPH: ${_cs_org_missing} not found under CS_ORG_ROOT=${CS_ORG_ROOT}; building without the render graph")
    else()
        # Vulkan backend only: CS reaches the GPU through DXVK, never through D3D12.
        # Streamline, PIX and Tracy GPU zones belong to the host (CS owns Streamline).
        set(BASICRHI_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(BASICRHI_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(BASICRHI_ENABLE_STREAMLINE OFF CACHE BOOL "" FORCE)
        set(BASICRHI_ENABLE_PIX OFF CACHE BOOL "" FORCE)
        set(BASICRHI_ENABLE_RESHAPE OFF CACHE BOOL "" FORCE)
        set(BASICRHI_ENABLE_TRACY_GPU_PROFILING OFF CACHE BOOL "" FORCE)
        set(BASICRHI_ENABLE_IMGUI OFF CACHE BOOL "" FORCE)
        set(BASICRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        # CS links the compiled spdlog library (SPDLOG_COMPILED_LIB); header-only copies would collide.
        set(BASICRHI_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
        set(OPENRENDERGRAPH_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
        set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(BASICTELEMETRY_BUILD_ARTIFACTS OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_ENABLE_SUBMODULE_FALLBACK ON CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_FORWARD_BASICRHI_DEP_OPTIONS OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_ENABLE_D3D11_INTEROP OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)

        add_subdirectory("${CS_ORG_ROOT}/OpenRenderGraph" "${CMAKE_BINARY_DIR}/extern/OpenRenderGraph" EXCLUDE_FROM_ALL)

        # SPIR-V for the render-graph passes, with the descriptor-heap ABI BasicRHI expects.
        include("${CS_ORG_ROOT}/BasicRHI/cmake/BasicRHIShaderFlags.cmake")
        set(_llf_shader_dir "${CMAKE_SOURCE_DIR}/features/Light Limit Fix/Shaders")
        foreach(_shader IN ITEMS ClusterBuildingCS ClusterCullingCS)
            set(_spv "${CS_GENERATED_SHADER_DIR}/LightLimitFix/ORG/${_shader}.spv")
            basicrhi_compile_spirv(
                OUTPUT "${_spv}"
                SOURCE "${_llf_shader_dir}/LightLimitFix/${_shader}.hlsl"
                ENTRY main
                PROFILE cs_6_6
                DEFINES LLF_ORG_BINDLESS=1
                INCLUDE_DIRS "${_llf_shader_dir}" "${CMAKE_SOURCE_DIR}/package/Shaders"
                DEPENDS
                    "${_llf_shader_dir}/LightLimitFix/Common.hlsli"
                    "${_llf_shader_dir}/LightLimitFix/OrgBindless.hlsli")
            list(APPEND CS_RENDER_GRAPH_SPIRV "${_spv}")
        endforeach()
        add_custom_target(CSRenderGraphShaders DEPENDS ${CS_RENDER_GRAPH_SPIRV})
        add_dependencies(${PROJECT_NAME} CSRenderGraphShaders)

        target_link_libraries(${PROJECT_NAME} PRIVATE OpenRenderGraph::OpenRenderGraph BasicRHI::BasicRHI)
        target_compile_definitions(${PROJECT_NAME} PRIVATE CS_HAS_RENDER_GRAPH=1)

        install(
            FILES ${CS_RENDER_GRAPH_SPIRV}
            DESTINATION Shaders/LightLimitFix/ORG
            COMPONENT Shaders
        )

        set(CS_HAS_RENDER_GRAPH ON)
        message(STATUS "CS_RENDER_GRAPH: OpenRenderGraph from ${CS_ORG_ROOT}")
    endif()
endif()
