cmake_minimum_required(VERSION 3.1)
project(OSP360)

option(OSPRAY_MODULE_OPENVR "OSPRay OpenVR Module" OFF)

if (OSPRAY_MODULE_OPENVR)
  set(CMAKE_MODULE_PATH "${OSP360_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
  set(CMAKE_CXX_STANDARD 11)

  find_package(GLM REQUIRED)
  find_package(OpenGL REQUIRED)
  find_package(Threads REQUIRED)
  find_package(SDL2 REQUIRED)
  find_package(OpenVR)

  add_definitions(-DGLM_FORCE_RADIANS)
  set(INCLUDE_DIRS ${SDL2_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR}
    ${GLM_INCLUDE_DIRS} ${OSP360_SOURCE_DIR})
  set(LIBS ${SDL2_LIBRARY} ${OPENGL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

  if (${OPENVR_FOUND})
    set(INCLUDE_DIRS ${INCLUDE_DIRS} ${OPENVR_INCLUDE_DIR})
    set(LIBS ${LIBS} ${OPENVR_LIBRARY})
    add_definitions(-DOPENVR_ENABLED)
  endif()

  include_directories(${INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    common/gl3w
    common/imgui
    ${CMAKE_SOURCE_DIR}/apps/exampleViewer
    )

  ospray_create_application(osp360
    main.cpp
    openvr_display.cpp
    gldebug.cpp
    gl3w.c
  LINK
    ospray
    ${LIBS}
    ospray
    ospray_common
    ospray_sg
    )
endif()

