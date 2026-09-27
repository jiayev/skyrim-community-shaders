include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../extern/ORGModuleServices/cmake/AsyncPrimitives.cmake)
find_package(Threads REQUIRED)
add_library(DclfSceneExecutor STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/FaceCapture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/FaceCapture.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CaptureService.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CaptureService.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CapturePreparation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CapturePreparation.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CaptureAdmission.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CaptureAdmission.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CapturedScene.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/CapturedScene.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/PublishedSceneExecutor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Features/DrawcallLimitFix/PublishedSceneExecutor.h)
target_compile_features(DclfSceneExecutor PUBLIC cxx_std_20)
target_include_directories(DclfSceneExecutor PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(DclfSceneExecutor PUBLIC ORGModuleServices::AsyncPrimitives Threads::Threads)
