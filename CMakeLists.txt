cmake_minimum_required(VERSION 3.24)
project(onegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(FetchGenICam)

add_library(onegen
  src/match.cpp
  src/gentl.cpp
  src/defaults.cpp
  src/node.cpp
  src/onegen.cpp
  src/camera.cpp
)
target_include_directories(onegen
  PUBLIC ${CMAKE_SOURCE_DIR}/include
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(onegen PUBLIC genicam::genicam)
if(UNIX)
  target_link_libraries(onegen PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(test_open_query tests/test_open_query.cpp)
target_include_directories(test_open_query PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_open_query PRIVATE onegen)

add_executable(test_node tests/test_node.cpp)
target_include_directories(test_node PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_node PRIVATE onegen)

add_custom_target(check COMMAND test_open_query COMMAND test_node)

add_executable(dump_gentl tools/dump_gentl.cpp)
target_include_directories(dump_gentl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dump_gentl PRIVATE onegen)

file(GLOB _qt_prefix "/opt/Qt/*/macos" "/opt/Qt/*/gcc_64" "/opt/Qt/*/msvc*")
list(APPEND CMAKE_PREFIX_PATH ${_qt_prefix} /opt /opt/Qt)
find_package(Qt6 QUIET COMPONENTS Widgets)
if(Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  add_executable(onegen-viewer
    apps/viewer/main.cpp
    apps/viewer/MainWindow.cpp
    apps/viewer/MainWindow.hpp
    apps/viewer/FeatureTreeWidget.cpp
    apps/viewer/FeatureTreeWidget.hpp
  )
  target_link_libraries(onegen-viewer PRIVATE onegen Qt6::Widgets)
else()
  message(STATUS "Qt6 not found under /opt — skipping onegen-viewer")
endif()
