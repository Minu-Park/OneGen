cmake_minimum_required(VERSION 3.24)
project(onegen LANGUAGES CXX)

option(ONEGEN_BUILD_TESTS "Build OneGen tests" ${PROJECT_IS_TOP_LEVEL})
option(ONEGEN_BUILD_TOOLS "Build OneGen command-line tools" ${PROJECT_IS_TOP_LEVEL})
option(ONEGEN_BUILD_VIEWER "Build the OneGen Qt viewer when Qt is available" ${PROJECT_IS_TOP_LEVEL})
option(ONEGEN_BUILD_WORKER "Build the isolated GenTL probe worker" OFF)
option(ONEGEN_BUILD_PLAYGROUND_ADAPTER "Build the Playground GraphicsFrame adapter" OFF)
option(ONEGEN_BUILD_PLAYGROUND_PLUGIN "Build the Playground device plugin" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchGenICam)

add_library(onegen
  src/match.cpp
  src/gentl.cpp
  src/defaults.cpp
  src/node.cpp
  src/onegen.cpp
  src/camera.cpp
)
add_library(OneGen::OneGen ALIAS onegen)
target_compile_features(onegen PUBLIC cxx_std_20)
target_include_directories(onegen
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(onegen PUBLIC genicam::genicam)
if(UNIX)
  target_link_libraries(onegen PRIVATE ${CMAKE_DL_LIBS})
endif()

if(ONEGEN_BUILD_TESTS)
  enable_testing()
  add_executable(test_open_query tests/test_open_query.cpp)
  target_include_directories(test_open_query PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_open_query PRIVATE onegen)
  add_test(NAME test_open_query COMMAND test_open_query)

  add_executable(test_node tests/test_node.cpp)
  target_include_directories(test_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_node PRIVATE onegen)
  add_test(NAME test_node COMMAND test_node)

  add_custom_target(check COMMAND test_open_query COMMAND test_node)
endif()

if(ONEGEN_BUILD_TOOLS)
  add_executable(dump_gentl tools/dump_gentl.cpp)
  target_include_directories(dump_gentl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(dump_gentl PRIVATE onegen)
endif()

file(GLOB _qt_prefix "/opt/Qt/*/macos" "/opt/Qt/*/gcc_64" "/opt/Qt/*/msvc*")
list(APPEND CMAKE_PREFIX_PATH ${_qt_prefix} /opt /opt/Qt)
if(ONEGEN_BUILD_VIEWER OR ONEGEN_BUILD_WORKER OR ONEGEN_BUILD_PLAYGROUND_PLUGIN)
  find_package(Qt6 QUIET COMPONENTS Widgets Network)
endif()
if(ONEGEN_BUILD_PLAYGROUND_PLUGIN AND Qt6_FOUND)
  find_package(Qt6 REQUIRED COMPONENTS Concurrent)
endif()

if(ONEGEN_BUILD_WORKER)
  if(NOT Qt6_FOUND)
    message(FATAL_ERROR "ONEGEN_BUILD_WORKER requires Qt6 Core and Network")
  endif()
  add_library(OneGenWorkerClient STATIC
    Utility/Worker/OneGenCoordinator.cpp
    Utility/Worker/OneGenCoordinator.h
    Utility/Worker/OneGenWorkerClient.cpp
    Utility/Worker/OneGenWorkerClient.h
    Utility/Worker/OneGenWorkerProtocol.h)
  add_library(OneGen::WorkerClient ALIAS OneGenWorkerClient)
  target_include_directories(OneGenWorkerClient PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Worker")
  target_link_libraries(OneGenWorkerClient PUBLIC onegen Qt6::Core Qt6::Network)

  add_executable(OneGenWorker
    Utility/Worker/OneGenWorkerMain.cpp
    Utility/Worker/OneGenWorkerProtocol.h)
  target_include_directories(OneGenWorker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Worker")
  target_link_libraries(OneGenWorker PRIVATE onegen Qt6::Core Qt6::Network)

  if(ONEGEN_BUILD_TESTS)
    add_executable(test_worker tests/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE OneGen::WorkerClient)
    add_test(NAME test_worker COMMAND test_worker $<TARGET_FILE:OneGenWorker>)
  endif()
endif()
if(ONEGEN_BUILD_VIEWER AND Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  add_executable(onegen-viewer
    apps/viewer/main.cpp
    apps/viewer/MainWindow.cpp
    apps/viewer/MainWindow.hpp
    apps/viewer/FeatureTreeWidget.cpp
    apps/viewer/FeatureTreeWidget.hpp
  )
  target_link_libraries(onegen-viewer PRIVATE onegen Qt6::Widgets)
elseif(ONEGEN_BUILD_VIEWER)
  message(STATUS "Qt6 not found under /opt — skipping onegen-viewer")
endif()

if(ONEGEN_BUILD_PLAYGROUND_ADAPTER)
  if(NOT TARGET GraphicsEngine::Contracts)
    message(FATAL_ERROR "ONEGEN_BUILD_PLAYGROUND_ADAPTER requires GraphicsEngine::Contracts")
  endif()
  add_library(OneGenPlaygroundAdapter STATIC
    Utility/PlaygroundAdapter/OneGenGraphicsFrameAdapter.cpp)
  add_library(OneGen::PlaygroundAdapter ALIAS OneGenPlaygroundAdapter)
  target_include_directories(OneGenPlaygroundAdapter PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter")
  target_link_libraries(OneGenPlaygroundAdapter PUBLIC onegen GraphicsEngine::Contracts)

  if(ONEGEN_BUILD_TESTS)
    add_executable(onegen_graphics_frame_adapter_test
      Utility/PlaygroundAdapter/Tests/GraphicsFrameAdapterTest.cpp)
    target_link_libraries(onegen_graphics_frame_adapter_test PRIVATE OneGen::PlaygroundAdapter)
    add_test(NAME onegen_graphics_frame_adapter_test COMMAND onegen_graphics_frame_adapter_test)
  endif()
endif()

if(ONEGEN_BUILD_PLAYGROUND_PLUGIN)
  if(NOT ONEGEN_BUILD_PLAYGROUND_ADAPTER OR NOT TARGET Playground::DevicePlugin
      OR NOT TARGET OneGen::WorkerClient OR NOT TARGET Resources::Resources OR NOT Qt6_FOUND)
    message(FATAL_ERROR "ONEGEN_BUILD_PLAYGROUND_PLUGIN requires the adapter, worker, Playground::DevicePlugin, Resources::Resources, and Qt6::Widgets")
  endif()
  add_library(OneGenPlugin MODULE
    Utility/PlaygroundAdapter/Plugin/OneGenPlugin.cpp
    Utility/PlaygroundAdapter/Source/OneGenSourceController.cpp
    Utility/PlaygroundAdapter/Source/OneGenSourceController.h
    Utility/Qt/OneGenControlWidget.cpp
    Utility/Qt/OneGenControlWidget.h
    apps/viewer/FeatureTreeWidget.cpp
    apps/viewer/FeatureTreeWidget.hpp)
  set_target_properties(OneGenPlugin PROPERTIES
    PREFIX "" OUTPUT_NAME OneGenPlugin AUTOMOC ON
    PLAYGROUND_PLUGIN_PACKAGE
      "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake")
  target_include_directories(OneGenPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/viewer"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Worker"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Qt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source")
  target_link_libraries(OneGenPlugin PRIVATE
    Playground::DevicePlugin OneGen::PlaygroundAdapter OneGen::WorkerClient Resources::Resources
    Qt6::Widgets Qt6::Concurrent)

  set(_onegen_runtime_payload "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
  file(MAKE_DIRECTORY "${_onegen_runtime_payload}")
  foreach(_runtime_file ${ONEGEN_GENICAM_RUNTIME_FILES})
    file(COPY "${_runtime_file}" DESTINATION "${_onegen_runtime_payload}")
  endforeach()
  set_target_properties(OneGenPlugin PROPERTIES
    PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR "${_onegen_runtime_payload}"
    PLAYGROUND_PLUGIN_RUNTIME_DEPENDENCY_DEST "runtime")

  set(_onegen_package_payload "${CMAKE_CURRENT_BINARY_DIR}/plugin-package")
  file(REMOVE_RECURSE "${_onegen_package_payload}")
  file(MAKE_DIRECTORY "${_onegen_package_payload}/cti")
  file(WRITE "${_onegen_package_payload}/cti/.keep" "")
  set_target_properties(OneGenPlugin PROPERTIES
    PLAYGROUND_PLUGIN_PACKAGE_PAYLOAD_DIR "${_onegen_package_payload}"
    PLAYGROUND_PLUGIN_AUXILIARY_TARGETS OneGenWorker)
endif()
