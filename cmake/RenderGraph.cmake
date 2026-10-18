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
set(CS_HAS_ORG_MODULE_SERVICES OFF)
set(CS_DXC_RUNTIME_DLLS)


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
        set(BASICRHI_ENABLE_TRACY_CPU_PROFILING ${TRACY_SUPPORT} CACHE BOOL "" FORCE)
        set(BASICRHI_ENABLE_IMGUI OFF CACHE BOOL "" FORCE)
        set(BASICRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        # CS links the compiled spdlog library (SPDLOG_COMPILED_LIB); header-only copies would collide.
        set(BASICRHI_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
        set(OPENRENDERGRAPH_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
        set(ORG_ASYNC_STATE_GRAPH_SPDLOG_TARGET spdlog::spdlog CACHE STRING "" FORCE)
        set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(BASICTELEMETRY_BUILD_ARTIFACTS OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_ENABLE_SUBMODULE_FALLBACK ON CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_FORWARD_BASICRHI_DEP_OPTIONS OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_ENABLE_D3D11_INTEROP OFF CACHE BOOL "" FORCE)
        set(OPENRENDERGRAPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)

        # Nsight Perf for OpenRenderGraph's Telemetry/NvPerfCapture (NvPerf's Vulkan backend): ORG_ENABLE_NVPERF and the
        # NVPerf::Host target come from CMakeLists.txt's NVPERF_SUPPORT block, which also delay-loads and installs the DLL.
        add_subdirectory("${CS_ORG_ROOT}/OpenRenderGraph" "${CMAKE_BINARY_DIR}/extern/OpenRenderGraph" EXCLUDE_FROM_ALL)

        # Runtime SPIR-V compilation (every render-graph shader): ORGModuleServices' content-addressed DXC service.
        # Optional; without it the render-graph features stay on their D3D11 paths.
        if(EXISTS "${CS_ORG_ROOT}/ORGModuleServices/CMakeLists.txt")
            set(ORG_MODULE_SERVICES_ENABLE_DXC ON CACHE BOOL "" FORCE)
            set(ORG_MODULE_SERVICES_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
            set(ORG_MODULE_SERVICES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory("${CS_ORG_ROOT}/ORGModuleServices" "${CMAKE_BINARY_DIR}/extern/ORGModuleServices" EXCLUDE_FROM_ALL)
            target_link_libraries(${PROJECT_NAME} PRIVATE ORGModuleServices::ORGModuleServices)
            target_compile_definitions(${PROJECT_NAME} PRIVATE CS_HAS_ORG_MODULE_SERVICES=1)
            set(CS_HAS_ORG_MODULE_SERVICES ON)
            # The Vulkan SDK's DXC: it has SPIR-V code generation (the Windows SDK's does not).
            foreach(_dll IN ITEMS dxcompiler.dll dxil.dll)
                if(EXISTS "$ENV{VULKAN_SDK}/Bin/${_dll}")
                    list(APPEND CS_DXC_RUNTIME_DLLS "$ENV{VULKAN_SDK}/Bin/${_dll}")
                endif()
            endforeach()
            if(NOT CS_DXC_RUNTIME_DLLS)
                message(WARNING "CS_RENDER_GRAPH: VULKAN_SDK has no dxcompiler.dll; runtime SPIR-V compilation will be unavailable in game")
            endif()
        endif()

        # The render-graph compute passes compile their HLSL at runtime through ORGModuleServices (src/RenderGraph/
        # ComputeProgram.cpp), like Drawcall Limit Fix's Lighting builds; the sources ship with the other shaders.
        target_link_libraries(${PROJECT_NAME} PRIVATE OpenRenderGraph::OpenRenderGraph BasicRHI::BasicRHI)
        # Unbounded lock-free queues (tbb::concurrent_queue): the render graph's compute submitter. Static, as DCLF's executor's.
        if(NOT TARGET TBB::tbb)
            find_package(TBB CONFIG REQUIRED)
        endif()
        target_link_libraries(${PROJECT_NAME} PRIVATE TBB::tbb)
        target_compile_definitions(${PROJECT_NAME} PRIVATE CS_HAS_RENDER_GRAPH=1)

        set(CS_HAS_RENDER_GRAPH ON)
        message(STATUS "CS_RENDER_GRAPH: OpenRenderGraph from ${CS_ORG_ROOT}")
    endif()
endif()

# Drawcall Limit Fix needs both (src/Features/DrawcallLimitFix.h). With them it is built and CS_HAS_DCLF is defined; without
# them its tree (src/Features/DrawcallLimitFix/) is left out of the build, and its shell, the one file that tests CS_HAS_DCLF,
# is inert.
get_target_property(_cs_dclf_plugin_sources ${PROJECT_NAME} SOURCES)
if(CS_HAS_RENDER_GRAPH AND CS_HAS_ORG_MODULE_SERVICES)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CS_HAS_DCLF=1)
    include("${CS_ORG_ROOT}/ORGModuleServices/cmake/AsyncPrimitives.cmake")
    target_link_libraries(${PROJECT_NAME} PRIVATE ORGModuleServices::AsyncPrimitives)
    include(${CMAKE_CURRENT_LIST_DIR}/DclfSceneExecutor.cmake)
    target_link_libraries(${PROJECT_NAME} PRIVATE DclfSceneExecutor)
    # DCLF's CPU publication graph shares ORGModuleServices' scheduler/runtime; it is linked explicitly and can be exercised
    # without a live frame switch.
    include(${CMAKE_CURRENT_LIST_DIR}/DclfSceneGraph.cmake)
    target_link_libraries(${PROJECT_NAME} PRIVATE DclfSceneGraph)
    # The plugin's source glob also finds these two; they are compiled only in their independently testable libraries, without
    # the game's PCH.
    list(FILTER _cs_dclf_plugin_sources EXCLUDE REGEX "/(SceneExecutor|SceneGraph)\.cpp$")
    message(STATUS "Drawcall Limit Fix: built")
else()
    list(FILTER _cs_dclf_plugin_sources EXCLUDE REGEX "/src/Features/DrawcallLimitFix/")
    message(STATUS "Drawcall Limit Fix: not built (it needs the render graph and ORGModuleServices)")
endif()
set_property(TARGET ${PROJECT_NAME} PROPERTY SOURCES ${_cs_dclf_plugin_sources})
