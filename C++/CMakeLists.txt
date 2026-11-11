option(ONEGEN_BUILD_TESTS "Build OneGen tests" ${PROJECT_IS_TOP_LEVEL})
option(ONEGEN_BUILD_QT_WIDGET "Build the optional OneGen Qt widget target." OFF)
option(ONEGEN_BUILD_PLAYGROUND_ADAPTER "Build the Playground GraphicsFrame adapter" OFF)
option(ONEGEN_BUILD_PLAYGROUND_PLUGIN "Build the Playground device plugin" OFF)

if(ONEGEN_BUILD_PLAYGROUND_PLUGIN)
  set(ONEGEN_BUILD_QT_WIDGET ON)
  set(ONEGEN_BUILD_PLAYGROUND_ADAPTER ON)
endif()
if(PROJECT_NAME STREQUAL "OneGen" AND PROJECT_IS_TOP_LEVEL AND
    (ONEGEN_BUILD_QT_WIDGET OR ONEGEN_BUILD_PLAYGROUND_ADAPTER OR ONEGEN_BUILD_PLAYGROUND_PLUGIN))
  find_package(PlaygroundPluginSDK 0.2 CONFIG REQUIRED)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(FetchGenICam)

add_library(onegen
  OneGen.cpp
  OneGenCamera.cpp
  OneGenNode.cpp
  Internal/Gentl.cpp
  Internal/ModuleGuard.cpp
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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(onegen PUBLIC rt)
endif()

if(ONEGEN_BUILD_TESTS)
  add_library(FailingInitProducer SHARED Tests/FailingInitProducer.cpp)
  target_compile_features(FailingInitProducer PRIVATE cxx_std_20)
  target_include_directories(FailingInitProducer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(FailingInitProducer PRIVATE genicam::genicam)
  set_target_properties(FailingInitProducer PROPERTIES PREFIX "" SUFFIX ".cti")

  add_library(SyntheticGenTL SHARED Tests/SyntheticGenTL.cpp)
  target_compile_features(SyntheticGenTL PRIVATE cxx_std_20)
  target_include_directories(SyntheticGenTL PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(SyntheticGenTL PRIVATE genicam::genicam)
  set_target_properties(SyntheticGenTL PROPERTIES PREFIX "" SUFFIX ".cti")
  add_executable(test_gentl_init Tests/test_gentl_init.cpp)
  target_link_libraries(test_gentl_init PRIVATE onegen)
  add_dependencies(test_gentl_init FailingInitProducer)
  add_test(NAME test_gentl_init COMMAND test_gentl_init $<TARGET_FILE:FailingInitProducer>)
  target_compile_options(test_gentl_init PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)

  add_executable(test_open_query Tests/test_open_query.cpp)
  target_link_libraries(test_open_query PRIVATE onegen)
  add_test(NAME test_open_query COMMAND test_open_query)

  add_executable(test_node Tests/test_node.cpp)
  target_link_libraries(test_node PRIVATE onegen)
  add_test(NAME test_node COMMAND test_node)

  add_executable(test_pinned_acquisition Tests/test_pinned_acquisition.cpp)
  target_link_libraries(test_pinned_acquisition PRIVATE onegen)
  add_test(NAME test_pinned_acquisition COMMAND test_pinned_acquisition)

  add_executable(test_pixel_layout Tests/test_pixel_layout.cpp)
  target_link_libraries(test_pixel_layout PRIVATE onegen)
  add_test(NAME test_pixel_layout COMMAND test_pixel_layout)

  add_library(GuardProbe SHARED Tests/GuardProbe.cpp)
  add_executable(test_module_guard Tests/test_module_guard.cpp)
  target_link_libraries(test_module_guard PRIVATE onegen ${CMAKE_DL_LIBS})
  add_dependencies(test_module_guard GuardProbe)
  if(MSVC)
    add_library(GuardDependency SHARED Tests/GuardDependency.cpp)
    set_target_properties(GuardDependency PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_CURRENT_BINARY_DIR}/guard_dependency>)
    add_library(GuardDelayed SHARED Tests/GuardDelayed.cpp)
    set_target_properties(GuardDelayed PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_CURRENT_BINARY_DIR}/guard_delayed>)
    target_link_libraries(GuardDelayed PRIVATE GuardDependency delayimp)
    target_link_options(GuardDelayed PRIVATE /DELAYLOAD:GuardDependency.dll)
    add_dependencies(test_module_guard GuardDelayed)
    add_test(NAME test_module_guard COMMAND test_module_guard $<TARGET_FILE:GuardProbe>
      $<TARGET_FILE:GuardDelayed> $<TARGET_FILE:GuardDependency>)
  else()
    add_test(NAME test_module_guard COMMAND test_module_guard $<TARGET_FILE:GuardProbe>)
  endif()
  foreach(_test test_open_query test_node test_pinned_acquisition
      test_pixel_layout test_module_guard)
    target_compile_options(${_test} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
  endforeach()

  if(ONEGEN_GENICAM_RUNTIME_FILES)
    list(GET ONEGEN_GENICAM_RUNTIME_FILES 0 _onegen_core_runtime_file)
    get_filename_component(_onegen_core_runtime_dir "${_onegen_core_runtime_file}" DIRECTORY)
    set_tests_properties(test_open_query test_node test_gentl_init test_pinned_acquisition PROPERTIES ENVIRONMENT_MODIFICATION
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

# In-process producer coordination: discovery, device leases and the frame slot pool.
add_library(OneGenProducer STATIC
  Utility/Producer/FramePool.cpp
  Utility/Producer/FramePool.h
  Utility/Producer/ParameterTypes.h
  Utility/Producer/ProducerCoordinator.cpp
  Utility/Producer/ProducerCoordinator.h)
add_library(OneGen::Producer ALIAS OneGenProducer)
target_include_directories(OneGenProducer PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Producer")
target_link_libraries(OneGenProducer PUBLIC onegen)
if(ONEGEN_BUILD_TESTS)
  add_executable(test_frame_pool Tests/test_frame_pool.cpp)
  target_link_libraries(test_frame_pool PRIVATE OneGen::Producer)
  add_test(NAME test_frame_pool COMMAND test_frame_pool)
  add_executable(test_producer Tests/test_producer.cpp)
  target_link_libraries(test_producer PRIVATE OneGen::Producer)
  add_dependencies(test_producer SyntheticGenTL FailingInitProducer)
  add_test(NAME test_producer COMMAND test_producer
    $<TARGET_FILE:SyntheticGenTL> $<TARGET_FILE:FailingInitProducer>)
  foreach(_test test_frame_pool test_producer)
    target_compile_options(${_test} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
  endforeach()
  set_tests_properties(test_producer PROPERTIES TIMEOUT 300)
  if(ONEGEN_GENICAM_RUNTIME_FILES)
    set_tests_properties(test_producer PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_onegen_core_runtime_dir}")
  endif()
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
  if(ONEGEN_BUILD_TESTS)
    add_executable(test_logging Tests/test_logging.cpp)
    target_link_libraries(test_logging PRIVATE onegen Qt6::Core)
    target_compile_options(test_logging PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    add_test(NAME test_logging COMMAND test_logging)
    set_tests_properties(test_logging PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
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
  endif()
endif()

if(ONEGEN_BUILD_QT_WIDGET)
  if(ONEGEN_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
  endif()
  if(NOT Qt6_FOUND)
    message(FATAL_ERROR "[OneGen] ONEGEN_BUILD_QT_WIDGET=ON requires Qt 6.")
  endif()
  if(NOT TARGET Playground::DevicePlugin)
    message(FATAL_ERROR "[OneGen] ONEGEN_BUILD_QT_WIDGET=ON requires Playground::DevicePlugin.")
  endif()
  if(NOT TARGET Resources::Widgets)
    message(FATAL_ERROR "[OneGen] ONEGEN_BUILD_QT_WIDGET=ON requires the Qt-only Resources::Widgets target.")
  endif()
  add_subdirectory(Utility/Qt)
  add_library(OneGenQtWidget STATIC
    Utility/Qt/OneGenDialogs.cpp
    Utility/Qt/OneGenDialogs.h
    Utility/Qt/QOneGenDeviceWidget.cpp
    Utility/Qt/QOneGenDeviceWidget.h
    Utility/Qt/QOneGenWidget.cpp
    Utility/Qt/QOneGenWidget.h
    Utility/Qt/NumericFeatureEditor.h
    Utility/Qt/OneGenLogSink.h
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
      OneGen::Producer
      Playground::DevicePlugin
      Resources::Widgets
      Qt6::Widgets
      Qt6::Concurrent)
  if(ONEGEN_BUILD_TESTS AND TARGET Qt6::Test AND ONEGEN_BUILD_PLAYGROUND_ADAPTER)
    enable_testing()
    qt_add_executable(OneGenFeatureTreeTests
      Utility/Qt/OneGenFeatureTreeTests.cpp
      Utility/PlaygroundAdapter/Source/OneGenSourceController.cpp
      Utility/PlaygroundAdapter/Source/OneGenParameterControl.cpp
      Utility/PlaygroundAdapter/Source/OneGenSourceController.h)
    set_target_properties(OneGenFeatureTreeTests PROPERTIES AUTOMOC ON)
    target_link_libraries(OneGenFeatureTreeTests PRIVATE OneGen::QtWidget OneGen::PlaygroundAdapter Qt6::Test)
    if(TARGET Resources::Resources)
      target_link_libraries(OneGenFeatureTreeTests PRIVATE Resources::Resources)
      target_compile_definitions(OneGenFeatureTreeTests PRIVATE ONEGEN_FEATURE_THEME=1)
    endif()
    add_test(NAME OneGenFeatureTreeTests COMMAND OneGenFeatureTreeTests -o -,txt)
    set_tests_properties(OneGenFeatureTreeTests PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
  endif()
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
      OR NOT TARGET Resources::Widgets OR NOT TARGET Resources::Dialogs OR NOT Qt6_FOUND)
    message(FATAL_ERROR "ONEGEN_BUILD_PLAYGROUND_PLUGIN requires the adapter, OneGen::QtWidget, Playground::DevicePlugin, Resources::Widgets, Resources::Dialogs, and Qt6::Widgets")
  endif()
  find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
  add_library(OneGenPlugin MODULE
    Utility/PlaygroundAdapter/Plugin/OneGenPlugin.cpp
    Utility/PlaygroundAdapter/Source/OneGenSourceController.cpp
    Utility/PlaygroundAdapter/Source/OneGenParameterControl.cpp
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
    Resources::Widgets Resources::Dialogs Qt6::Widgets Qt6::Concurrent)

  set(_onegen_runtime_payload "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
  file(MAKE_DIRECTORY "${_onegen_runtime_payload}")
  foreach(_runtime_file ${ONEGEN_GENICAM_RUNTIME_FILES})
    file(COPY "${_runtime_file}" DESTINATION "${_onegen_runtime_payload}")
  endforeach()
  set_target_properties(OneGenPlugin PROPERTIES
    PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR "${_onegen_runtime_payload}"
    PLAYGROUND_PLUGIN_RUNTIME_DEPENDENCY_DEST "runtime")
  if(PROJECT_NAME STREQUAL "OneGen" AND PROJECT_IS_TOP_LEVEL)
    if(APPLE)
      set_target_properties(OneGenPlugin PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@loader_path/runtime;@executable_path/../Frameworks")
    elseif(UNIX)
      set_target_properties(OneGenPlugin PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/runtime")
    endif()
    playground_sdk_package_plugin(OneGenPlugin
      PACKAGE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake"
      ICON "${PlaygroundPluginSDK_ICON_DIR}/genicam-48.png"
      RUNTIME_FILES ${ONEGEN_GENICAM_RUNTIME_FILES})
  endif()
  message(STATUS "[OneGen] OneGenPlugin enabled.")
  if(ONEGEN_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(test_plugin_discovery Tests/test_plugin_discovery.cpp)
    set_target_properties(test_plugin_discovery PROPERTIES AUTOMOC ON)
    target_link_libraries(test_plugin_discovery PRIVATE Playground::DevicePlugin Qt6::Widgets Qt6::Test)
    target_compile_definitions(test_plugin_discovery PRIVATE
      ONEGEN_TEST_PLUGIN="$<TARGET_FILE:OneGenPlugin>"
      ONEGEN_TEST_PRODUCER="$<TARGET_FILE:SyntheticGenTL>")
    add_dependencies(test_plugin_discovery OneGenPlugin SyntheticGenTL)
    add_test(NAME test_plugin_discovery COMMAND test_plugin_discovery)
    set_tests_properties(test_plugin_discovery PROPERTIES TIMEOUT 30
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
  endif()
endif()

if(TARGET OneGenQtWidget)
    target_link_libraries(OneGenQtWidget PUBLIC OneGenPresentation)
endif()
