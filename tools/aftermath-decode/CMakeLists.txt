# aftermath-decode: a standalone decoder for the Nsight Aftermath GPU crash dumps Community Shaders writes
# (src/Aftermath.cpp). Build it on its own:
#
#   cmake -S tools/aftermath-decode -B build/aftermath-decode
#   cmake --build build/aftermath-decode --config Release
#
# The SDK is found as the plugin's build finds it: -DAFTERMATH_SDK_DIR, the NSIGHT_AFTERMATH_SDK environment
# variable, or a vendored extern/aftermath. Its runtime DLL is copied next to the executable.
cmake_minimum_required(VERSION 3.24)
project(aftermath-decode LANGUAGES CXX)

set(AFTERMATH_SDK_DIR "" CACHE PATH "Root of the Nsight Aftermath SDK (the directory holding include/ and lib/).")
set(_candidates "${AFTERMATH_SDK_DIR}" "$ENV{NSIGHT_AFTERMATH_SDK}" "${CMAKE_CURRENT_SOURCE_DIR}/../../extern/aftermath")
set(_root "")
foreach(_candidate IN LISTS _candidates)
    if(_candidate AND EXISTS "${_candidate}/include/GFSDK_Aftermath.h")
        set(_root "${_candidate}")
        break()
    endif()
endforeach()
if(NOT _root)
    message(FATAL_ERROR "Nsight Aftermath SDK not found. Searched: ${_candidates}. Pass -DAFTERMATH_SDK_DIR.")
endif()

add_executable(aftermath-decode main.cpp)
target_compile_features(aftermath-decode PRIVATE cxx_std_20)
target_include_directories(aftermath-decode PRIVATE "${_root}/include")
target_link_libraries(aftermath-decode PRIVATE "${_root}/lib/x64/GFSDK_Aftermath_Lib.x64.lib")
add_custom_command(TARGET aftermath-decode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_root}/lib/x64/GFSDK_Aftermath_Lib.x64.dll" "$<TARGET_FILE_DIR:aftermath-decode>")
