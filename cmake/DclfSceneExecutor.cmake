include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../extern/ORGModuleServices/cmake/AsyncPrimitives.cmake)
find_package(Threads REQUIRED)
add_library(DclfSceneExecutor STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/FaceCapture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/FaceCapture.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CaptureService.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CaptureService.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CapturePreparation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CapturePreparation.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CaptureAdmission.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CaptureAdmission.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CapturedScene.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/CapturedScene.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/PublishedSceneExecutor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/Published/PublishedSceneExecutor.h)
target_compile_features(DclfSceneExecutor PUBLIC cxx_std_20)
target_include_directories(DclfSceneExecutor PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(DclfSceneExecutor PUBLIC ORGModuleServices::AsyncPrimitives Threads::Threads)
