cmake_minimum_required(VERSION 3.24)

project(
	CrashFixCompendium
	VERSION 1.2.0
	LANGUAGES CXX
)

list(APPEND CMAKE_IGNORE_PATH "${PROJECT_BINARY_DIR}")

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

add_subdirectory(External/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)
include(External/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
configure_file(cmake/version.rc.in version.rc @ONLY)

add_commonlibsse_plugin(
	${PROJECT_NAME}
	AUTHOR Yuof
	SOURCES ${SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
target_compile_options(
	${PROJECT_NAME}
	PRIVATE
		/sdl
		/utf-8
		/Zi
		/permissive-
		"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
)
target_link_options(
	${PROJECT_NAME}
	PRIVATE
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)

enable_testing()

add_executable(settings_test tests/settings_test.cpp src/settings.cpp)
target_compile_features(settings_test PRIVATE cxx_std_20)
target_include_directories(settings_test PRIVATE src)
target_compile_definitions(settings_test PRIVATE NOMINMAX)
target_compile_options(settings_test PRIVATE /utf-8)
add_test(NAME settings COMMAND settings_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
	add_test(NAME source COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/Tools/check_source.py")
endif()
