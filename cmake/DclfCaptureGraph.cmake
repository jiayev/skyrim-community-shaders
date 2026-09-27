include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/DclfSceneExecutor.cmake)
if(NOT TARGET ORGModuleServices::AsyncStateGraph)
    include(${CMAKE_CURRENT_LIST_DIR}/../extern/ORGModuleServices/cmake/AsyncStateGraph.cmake)
endif()
add_library(DclfCaptureGraph STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CaptureGraph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CaptureGraph.h)
target_link_libraries(DclfCaptureGraph PUBLIC DclfSceneExecutor PRIVATE ORGModuleServices::AsyncStateGraph)
target_compile_features(DclfCaptureGraph PUBLIC cxx_std_23)
