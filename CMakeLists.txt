cmake_minimum_required(VERSION 3.21)

project(
	CommunityShaders
	VERSION 0.4.2
	LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(XSEPlugin)

find_package(magic_enum CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

target_link_libraries(
	${PROJECT_NAME} 
	PRIVATE
		debug ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Debug/detours.lib
		optimized ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Release/detours.lib
		magic_enum::magic_enum
		xbyak::xbyak
		nlohmann_json::nlohmann_json
		imgui::imgui
)