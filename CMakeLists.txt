cmake_minimum_required(VERSION 3.1)
project(OSP360)

set(CMAKE_MODULE_PATH "${OSP360_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_CXX_STANDARD 11)

find_package(ospray REQUIRED)
find_package(GLM REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(GLFW REQUIRED)

add_definitions(-DGLM_FORCE_RADIANS)

include_directories(${OSPRAY_INCLUDE_DIRS} ${GLFW_INCLUDE_DIR}
	${OPENGL_INCLUDE_DIR} ${GLM_INCLUDE_DIRS} ${OSP360_SOURCE_DIR})
add_executable(osp360 main.cpp gl3w.c)
target_link_libraries(osp360 ${OSPRAY_LIBRARIES} ${GLFW_LIBRARY}
	${OPENGL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

