include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../extern/ORGModuleServices/cmake/AsyncPrimitives.cmake)
find_package(Threads REQUIRED)
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()
add_library(DclfSceneExecutor STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Runtime/SceneExecutor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Runtime/SceneExecutor.h)
target_compile_features(DclfSceneExecutor PUBLIC cxx_std_20)
target_include_directories(DclfSceneExecutor PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(DclfSceneExecutor PUBLIC ORGModuleServices::AsyncPrimitives Threads::Threads PRIVATE TBB::tbb)
