option(ONEGEN_BUILD_TESTS "Build OneGen tests" ${PROJECT_IS_TOP_LEVEL})
option(ONEGEN_BUILD_WORKER "Build the isolated GenTL worker" OFF)
option(ONEGEN_BUILD_QT_WIDGET "Build the optional OneGen Qt widget target." OFF)
option(ONEGEN_BUILD_PLAYGROUND_ADAPTER "Build the Playground GraphicsFrame adapter" OFF)
option(ONEGEN_BUILD_PLAYGROUND_PLUGIN "Build the Playground device plugin" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(FetchGenICam)

add_library(onegen
  OneGen.cpp
  OneGenCamera.cpp
  OneGenNode.cpp
  Internal/Gentl.cpp
  Internal/XmlHttp.cpp
  Internal/Match.cpp
  Internal/OneGenDefaults.cpp
)
add_library(OneGen::OneGen ALIAS onegen)
target_compile_features(onegen PUBLIC cxx_std_20)
target_include_directories(onegen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(onegen PUBLIC genicam::genicam)
if(WIN32)
  target_link_libraries(onegen PRIVATE winhttp)
else()
  find_package(CURL QUIET)
  if(CURL_FOUND)
    target_link_libraries(onegen PRIVATE CURL::libcurl)
    target_compile_definitions(onegen PRIVATE ONEGEN_HAVE_CURL=1)
  endif()
endif()
if(UNIX)
  target_link_libraries(onegen PRIVATE ${CMAKE_DL_LIBS})
endif()

if(ONEGEN_BUILD_TESTS)
  add_executable(test_open_query Tests/test_open_query.cpp)
  target_link_libraries(test_open_query PRIVATE onegen)
  add_test(NAME test_open_query COMMAND test_open_query)

  add_executable(test_node Tests/test_node.cpp)
  target_link_libraries(test_node PRIVATE onegen)
  add_test(NAME test_node COMMAND test_node)

  add_executable(test_worker_protocol Tests/test_worker_protocol.cpp)
  target_include_directories(test_worker_protocol PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Worker")
  target_link_libraries(test_worker_protocol PRIVATE onegen)
  add_test(NAME test_worker_protocol COMMAND test_worker_protocol)
  foreach(_test test_open_query test_node test_worker_protocol)
    target_compile_options(${_test} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
  endforeach()

  if(ONEGEN_GENICAM_RUNTIME_FILES)
    list(GET ONEGEN_GENICAM_RUNTIME_FILES 0 _onegen_core_runtime_file)
    get_filename_component(_onegen_core_runtime_dir "${_onegen_core_runtime_file}" DIRECTORY)
    set_tests_properties(test_open_query test_node PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_onegen_core_runtime_dir}")
  endif()

  add_custom_target(check COMMAND test_open_query COMMAND test_node)
  if(WIN32 OR CURL_FOUND)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_executable(test_xml_http Tests/test_xml_http.cpp)
      target_link_libraries(test_xml_http PRIVATE onegen)
      target_compile_options(test_xml_http PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
      add_test(NAME test_xml_http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_xml_http.py" $<TARGET_FILE:test_xml_http>)
      set_tests_properties(test_xml_http PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_onegen_core_runtime_dir}")
    endif()
  endif()
endif()

if(ONEGEN_BUILD_WORKER)
  add_executable(OneGenWorker
    Utility/Worker/OneGenWorkerMain.cpp
    Utility/Worker/OneGenWorkerMessages.h
    Utility/Worker/OneGenWorkerProtocol.h)
  target_include_directories(OneGenWorker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Worker")
  target_link_libraries(OneGenWorker PRIVATE onegen)
endif()

if(ONEGEN_BUILD_QT_WIDGET OR ONEGEN_BUILD_PLAYGROUND_PLUGIN)
  find_package(Qt6 QUIET COMPONENTS Core Widgets)
elseif(ONEGEN_BUILD_TESTS)
  find_package(Qt6 QUIET COMPONENTS Core)
endif()

if((ONEGEN_BUILD_TESTS OR ONEGEN_BUILD_QT_WIDGET OR ONEGEN_BUILD_PLAYGROUND_PLUGIN) AND Qt6_FOUND)
  if(ONEGEN_BUILD_QT_WIDGET OR ONEGEN_BUILD_PLAYGROUND_PLUGIN)
    find_package(Qt6 REQUIRED COMPONENTS Concurrent)
  endif()
  if(NOT ONEGEN_BUILD_WORKER)
    message(FATAL_ERROR "OneGen Qt support requires ONEGEN_BUILD_WORKER")
  endif()
  add_library(OneGenWorkerClient STATIC
    Utility/Worker/OneGenCoordinator.cpp
    Utility/Worker/OneGenCoordinator.h
    Utility/Worker/OneGenWorkerClient.cpp
    Utility/Worker/OneGenWorkerClient.h
    Utility/Worker/OneGenWorkerMessages.h
    Utility/Worker/OneGenWorkerProtocol.h)
  add_library(OneGen::WorkerClient ALIAS OneGenWorkerClient)
  target_include_directories(OneGenWorkerClient PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Worker")
  target_link_libraries(OneGenWorkerClient PUBLIC onegen Qt6::Core)
  if(ONEGEN_BUILD_TESTS)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(TARGET Qt6::Widgets)
      add_executable(test_numeric_editor Tests/test_numeric_editor.cpp
        Utility/Qt/NumericFeatureEditor.h)
      set_target_properties(test_numeric_editor PROPERTIES AUTOMOC ON)
      target_link_libraries(test_numeric_editor PRIVATE onegen Qt6::Widgets)
      target_compile_options(test_numeric_editor PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
      add_test(NAME test_numeric_editor COMMAND test_numeric_editor)
      set_tests_properties(test_numeric_editor PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
    endif()
    add_executable(test_worker Tests/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE OneGen::WorkerClient)
    target_compile_options(test_worker PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    add_test(NAME test_worker COMMAND test_worker $<TARGET_FILE:OneGenWorker>)
    add_test(NAME test_worker_package COMMAND ${CMAKE_COMMAND}
      "-DWORKER_FILE=$<TARGET_FILE:OneGenWorker>"
      "-DCMAKE_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_worker_package.cmake")
    if(ONEGEN_GENICAM_RUNTIME_FILES)
      list(GET ONEGEN_GENICAM_RUNTIME_FILES 0 _onegen_runtime_file)
      get_filename_component(_onegen_runtime_dir "${_onegen_runtime_file}" DIRECTORY)
      set_tests_properties(test_worker PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_runtime_dir}")
    endif()
  endif()
endif()

if(ONEGEN_BUILD_QT_WIDGET)
  if(NOT Qt6_FOUND)
    message(FATAL_ERROR "[OneGen] ONEGEN_BUILD_QT_WIDGET=ON requires Qt 6.")
  endif()
  if(NOT TARGET OneGen::WorkerClient)
    message(FATAL_ERROR "[OneGen] ONEGEN_BUILD_QT_WIDGET=ON requires OneGen::WorkerClient.")
  endif()
  if(NOT TARGET Playground::DevicePlugin)
    message(FATAL_ERROR "[OneGen] ONEGEN_BUILD_QT_WIDGET=ON requires Playground::DevicePlugin.")
  endif()
  add_library(OneGenQtWidget STATIC
    Utility/Qt/QOneGenDeviceWidget.cpp
    Utility/Qt/QOneGenDeviceWidget.h
    Utility/Qt/QOneGenWidget.cpp
    Utility/Qt/QOneGenWidget.h
    Utility/Qt/NumericFeatureEditor.h
  )
  add_library(OneGen::QtWidget ALIAS OneGenQtWidget)
  set_target_properties(OneGenQtWidget PROPERTIES AUTOMOC ON)
  target_compile_features(OneGenQtWidget PUBLIC cxx_std_20)
  target_include_directories(OneGenQtWidget PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Qt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source")
  target_link_libraries(OneGenQtWidget
    PUBLIC
      OneGen::WorkerClient
      Playground::DevicePlugin
      Qt6::Widgets
      Qt6::Concurrent)
  message(STATUS "[OneGen] OneGen::QtWidget enabled.")
endif()

if(ONEGEN_BUILD_PLAYGROUND_ADAPTER)
  if(NOT TARGET GraphicsEngine::Contracts)
    message(FATAL_ERROR "ONEGEN_BUILD_PLAYGROUND_ADAPTER requires GraphicsEngine::Contracts")
  endif()
  add_library(OneGenPlaygroundAdapter STATIC
    Utility/PlaygroundAdapter/OneGenGraphicsFrameAdapter.cpp)
  add_library(OneGen::PlaygroundAdapter ALIAS OneGenPlaygroundAdapter)
  target_include_directories(OneGenPlaygroundAdapter PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter")
  target_link_libraries(OneGenPlaygroundAdapter PUBLIC onegen GraphicsEngine::Contracts)

  if(ONEGEN_BUILD_TESTS)
    add_executable(onegen_graphics_frame_adapter_test
      Utility/PlaygroundAdapter/Tests/GraphicsFrameAdapterTest.cpp)
    target_link_libraries(onegen_graphics_frame_adapter_test PRIVATE OneGen::PlaygroundAdapter)
    target_compile_options(onegen_graphics_frame_adapter_test PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    add_test(NAME onegen_graphics_frame_adapter_test COMMAND onegen_graphics_frame_adapter_test)
  endif()
  message(STATUS "[OneGen] OneGen::PlaygroundAdapter enabled.")
endif()

if(ONEGEN_BUILD_PLAYGROUND_PLUGIN)
  if(NOT ONEGEN_BUILD_PLAYGROUND_ADAPTER OR NOT ONEGEN_BUILD_QT_WIDGET
      OR NOT TARGET Playground::DevicePlugin
      OR NOT TARGET Resources::Resources OR NOT Qt6_FOUND)
    message(FATAL_ERROR "ONEGEN_BUILD_PLAYGROUND_PLUGIN requires the adapter, OneGen::QtWidget, Playground::DevicePlugin, Resources::Resources, and Qt6::Widgets")
  endif()
  find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
  add_library(OneGenPlugin MODULE
    Utility/PlaygroundAdapter/Plugin/OneGenPlugin.cpp
    Utility/PlaygroundAdapter/Source/OneGenSourceController.cpp
    Utility/PlaygroundAdapter/Source/OneGenSourceController.h)
  set_target_properties(OneGenPlugin PROPERTIES
    PREFIX "" OUTPUT_NAME OneGenPlugin AUTOMOC ON
    PLAYGROUND_PLUGIN_PACKAGE
      "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake")
  target_include_directories(OneGenPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Plugin")
  target_link_libraries(OneGenPlugin PRIVATE
    Playground::DevicePlugin OneGen::PlaygroundAdapter OneGen::QtWidget
    Resources::Resources Qt6::Widgets Qt6::Concurrent)

  set(_onegen_runtime_payload "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
  file(MAKE_DIRECTORY "${_onegen_runtime_payload}")
  foreach(_runtime_file ${ONEGEN_GENICAM_RUNTIME_FILES})
    file(COPY "${_runtime_file}" DESTINATION "${_onegen_runtime_payload}")
  endforeach()
  set_target_properties(OneGenPlugin PROPERTIES
    PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR "${_onegen_runtime_payload}"
    PLAYGROUND_PLUGIN_RUNTIME_DEPENDENCY_DEST "runtime"
    PLAYGROUND_PLUGIN_AUXILIARY_TARGETS OneGenWorker)
  message(STATUS "[OneGen] OneGenPlugin enabled.")
endif()
