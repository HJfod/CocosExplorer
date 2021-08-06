cmake_minimum_required(VERSION 3.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(cocos-designer)

find_file(WINDOWS_HEADER windows.h)
if(NOT WINDOWS_HEADER)
  message(FATAL_ERROR "Can't find windows.h!")
endif()

file(GLOB_RECURSE IMGUI_FILES "libraries/imgui-hook/Universal OpenGL 2 Kiero Hook/**/*.cpp")
file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.h src/*.hpp)
add_library(cocos-designer SHARED ${SRC_FILES} ${IMGUI_FILES})

target_include_directories(cocos-designer PRIVATE
  libraries/minhook/include
  "libraries/imgui-hook/Universal OpenGL 2 Kiero Hook/include"
  "libraries/imgui-hook/Universal OpenGL 2 Kiero Hook/include/imgui"
  libraries/cocos-headers/cocos2dx/
  libraries/cocos-headers/cocos2dx/include
  libraries/cocos-headers/cocos2dx/kazmath/include
  libraries/cocos-headers/cocos2dx/platform/win32
  libraries/cocos-headers/cocos2dx/platform/third_party/win32
  libraries/cocos-headers/cocos2dx/platform/third_party/win32/OGLES
  libraries/cocos-headers/extensions/GUI/CCControlExtension
)

add_subdirectory(libraries/minhook)
target_link_libraries(cocos-designer minhook)
target_link_libraries(cocos-designer ${CMAKE_SOURCE_DIR}/libraries/cocos-headers/cocos2dx/libcocos2d.lib)
target_link_libraries(cocos-designer opengl32)
