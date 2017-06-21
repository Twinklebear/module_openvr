cmake_minimum_required(VERSION 3.1)
project(OSP360)

option(OSPRAY_MODULE_360_VIEWER "OSPRay 360 Viewer App" OFF)

if (OSPRAY_MODULE_360_VIEWER)
	set(CMAKE_MODULE_PATH "${OSP360_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
	set(CMAKE_CXX_STANDARD 11)

	find_package(GLM REQUIRED)
	find_package(OpenGL REQUIRED)
	find_package(Threads REQUIRED)
	find_package(SDL2 REQUIRED)
	find_package(OpenVR REQUIRED)

	add_definitions(-DGLM_FORCE_RADIANS)

	include_directories(${SDL2_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR}
		${GLM_INCLUDE_DIRS} ${OPENVR_INCLUDE_DIR} ${OSP360_SOURCE_DIR})

	ospray_create_application(osp360
		main.cpp
		openvr_display.cpp
		gldebug.cpp
		gl3w.c
	LINK
		ospray
		${SDL2_LIBRARY}
		${OPENVR_LIBRARY}
		${OPENGL_LIBRARIES}
		${CMAKE_THREAD_LIBS_INIT}
		${CMAKE_DL_LIBS})
endif()

