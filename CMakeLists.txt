cmake_minimum_required (VERSION 3.10)

project (zet139 VERSION 1.0.0 LANGUAGES C)

option(BUILD_SHARED "Build shared library" ON)
option(BUILD_STATIC "Build static library" ON)
option(BUILD_EXAMPLES "Build examples" ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ZET139_INCLUDE
	include/zet139.h
)

set(ZET139_SOURCES
	src/zet139.c
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMODBUS REQUIRED IMPORTED_TARGET libmodbus)

if(WIN32)
	set(PLATFORM_LIBS ws2_32 advapi32)
else()
	set(PLATFORM_LIBS pthread)
endif()

if(MSVC)
	add_definitions(-D_CRT_SECURE_NO_WARNINGS)
	add_compile_options(/utf-8)
endif()

if(BUILD_STATIC)
	add_library(zet139_static STATIC ${ZET139_SOURCES} ${ZET139_INCLUDE})
	target_include_directories(zet139_static PRIVATE include)
	target_link_libraries(zet139_static PRIVATE ${PLATFORM_LIBS} PkgConfig::LIBMODBUS)
	set_target_properties(zet139_static PROPERTIES
		OUTPUT_NAME "zet139_s"
		VERSION ${PROJECT_VERSION}
		SOVERSION ${PROJECT_VERSION_MAJOR}
		POSITION_INDEPENDENT_CODE ON
	)
endif()

if(BUILD_SHARED)
	add_library(zet139_shared SHARED ${ZET139_SOURCES} ${ZET139_INCLUDE})
	target_include_directories(zet139_shared PRIVATE include)
	target_link_libraries(zet139_shared PRIVATE ${PLATFORM_LIBS} PkgConfig::LIBMODBUS)
	if(WIN32)
		target_sources(zet139_shared PRIVATE win32/zet139.def)
	endif()
	set_target_properties(zet139_shared PROPERTIES
		OUTPUT_NAME "zet139"
		VERSION ${PROJECT_VERSION}
		SOVERSION ${PROJECT_VERSION_MAJOR}
		POSITION_INDEPENDENT_CODE ON
	)
endif()

if(BUILD_EXAMPLES)
	add_subdirectory(example)
endif()
