include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/DclfSceneExecutor.cmake)
if(NOT TARGET ORGModuleServices::AsyncStateGraph)
    include(${CMAKE_CURRENT_LIST_DIR}/../extern/ORGModuleServices/cmake/AsyncStateGraph.cmake)
endif()
add_library(DclfSceneGraph STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Runtime/SceneGraph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Runtime/SceneGraph.h)
target_link_libraries(DclfSceneGraph PUBLIC DclfSceneExecutor PRIVATE ORGModuleServices::AsyncStateGraph)
target_compile_features(DclfSceneGraph PUBLIC cxx_std_23)
