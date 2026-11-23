option(ONEGEN_BUILD_TESTS "Build OneGen tests" ${PROJECT_IS_TOP_LEVEL})
option(ONEGEN_BUILD_PLAYGROUND_ADAPTER "Build the Playground GraphicsFrame adapter" OFF)
option(ONEGEN_BUILD_PLAYGROUND_PLUGIN "Build the Playground device plugin" OFF)

if(ONEGEN_BUILD_PLAYGROUND_PLUGIN)
  set(ONEGEN_BUILD_PLAYGROUND_ADAPTER ON)
endif()
if(PROJECT_NAME STREQUAL "OneGen" AND PROJECT_IS_TOP_LEVEL AND
    (ONEGEN_BUILD_PLAYGROUND_ADAPTER OR ONEGEN_BUILD_PLAYGROUND_PLUGIN))
  find_package(PlaygroundPluginSDK 0.3 CONFIG REQUIRED)
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
  add_dependencies(test_gentl_init FailingInitProducer SyntheticGenTL)
  add_test(NAME test_gentl_init COMMAND test_gentl_init
    $<TARGET_FILE:FailingInitProducer> $<TARGET_FILE:SyntheticGenTL> $<TARGET_FILE:GuardProbe>)
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
  add_dependencies(test_gentl_init GuardProbe)
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

# Producer coordination: discovery, device leases and the pinned direct lease sink.
add_library(OneGenProducer STATIC
  Utility/Producer/DirectLeaseSink.cpp
  Utility/Producer/DirectLeaseSink.h
  Utility/Producer/ParameterTypes.h
  Utility/Producer/ProducerCoordinator.cpp
  Utility/Producer/ProducerCoordinator.h
  Utility/Producer/ProducerMessages.h)
add_library(OneGen::Producer ALIAS OneGenProducer)
target_include_directories(OneGenProducer PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/Utility/Producer")
target_link_libraries(OneGenProducer PUBLIC onegen)
if(ONEGEN_BUILD_TESTS)
  add_executable(test_direct_lease_sink Tests/test_direct_lease_sink.cpp)
  target_link_libraries(test_direct_lease_sink PRIVATE OneGen::Producer)
  add_test(NAME test_direct_lease_sink COMMAND test_direct_lease_sink)
  add_executable(test_producer Tests/test_producer.cpp)
  target_link_libraries(test_producer PRIVATE OneGen::Producer)
  add_dependencies(test_producer SyntheticGenTL FailingInitProducer)
  add_test(NAME test_producer COMMAND test_producer
    $<TARGET_FILE:SyntheticGenTL> $<TARGET_FILE:FailingInitProducer>)
  foreach(_test test_direct_lease_sink test_producer)
    target_compile_options(${_test} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
  endforeach()
  set_tests_properties(test_producer PROPERTIES TIMEOUT 300)
  if(MSVC)
    foreach(_variant 1 2)
      add_library(ConflictDependency${_variant} SHARED Tests/ConflictDependency.cpp)
      target_compile_definitions(ConflictDependency${_variant} PRIVATE ONEGEN_CONFLICT_VALUE=${_variant})
      set_target_properties(ConflictDependency${_variant} PROPERTIES OUTPUT_NAME ConflictDependency
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/conflict${_variant}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/conflict${_variant}")
      add_library(ConflictProducer${_variant} SHARED Tests/SyntheticGenTL.cpp)
      target_compile_features(ConflictProducer${_variant} PRIVATE cxx_std_20)
      target_compile_definitions(ConflictProducer${_variant} PRIVATE ONEGEN_CONFLICT_VALUE=${_variant})
      target_include_directories(ConflictProducer${_variant} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
      target_link_libraries(ConflictProducer${_variant} PRIVATE genicam::genicam ConflictDependency${_variant})
      set_target_properties(ConflictProducer${_variant} PROPERTIES PREFIX "" SUFFIX ".cti"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/conflict${_variant}")
    endforeach()
    add_executable(test_concurrent_producers Tests/test_concurrent_producers.cpp)
    target_link_libraries(test_concurrent_producers PRIVATE OneGen::Producer)
    target_compile_options(test_concurrent_producers PRIVATE /UNDEBUG)
    add_dependencies(test_concurrent_producers ConflictProducer1 ConflictProducer2)
    add_test(NAME test_concurrent_producers COMMAND test_concurrent_producers
      $<TARGET_FILE:ConflictProducer1> $<TARGET_FILE:ConflictProducer2>)
    set_tests_properties(test_concurrent_producers PROPERTIES TIMEOUT 30
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_onegen_core_runtime_dir}")
  endif()

  if(ONEGEN_GENICAM_RUNTIME_FILES)
    set_tests_properties(test_producer PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${_onegen_core_runtime_dir}")
  endif()
endif()

if(ONEGEN_BUILD_TESTS OR ONEGEN_BUILD_PLAYGROUND_PLUGIN)
  find_package(Qt6 QUIET COMPONENTS Core)
endif()

if((ONEGEN_BUILD_TESTS OR ONEGEN_BUILD_PLAYGROUND_PLUGIN) AND Qt6_FOUND)
  if(ONEGEN_BUILD_PLAYGROUND_PLUGIN)
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
  endif()
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
  if(NOT ONEGEN_BUILD_PLAYGROUND_ADAPTER OR NOT TARGET Playground::DeviceRuntimeContract OR NOT Qt6_FOUND)
    message(FATAL_ERROR "ONEGEN_BUILD_PLAYGROUND_PLUGIN requires the adapter, Playground::DeviceRuntimeContract and Qt6")
  endif()
  find_package(Qt6 REQUIRED COMPONENTS Concurrent)
  add_library(OneGenPlugin MODULE
    Utility/PlaygroundAdapter/Plugin/OneGenPlugin.cpp
    Utility/PlaygroundAdapter/Source/OneGenSourceController.cpp
    Utility/PlaygroundAdapter/Source/OneGenParameterControl.cpp
    Utility/PlaygroundAdapter/Source/OneGenTools.cpp
    Utility/PlaygroundAdapter/Source/OneGenSourceController.h)
  playground_sdk_add_runtime_contract(OneGenPlugin)
  set_target_properties(OneGenPlugin PROPERTIES
    PREFIX "" OUTPUT_NAME OneGenPlugin AUTOMOC ON
    PLAYGROUND_PLUGIN_PACKAGE
      "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake")
  target_include_directories(OneGenPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Plugin")
  target_link_libraries(OneGenPlugin PRIVATE
    Playground::DeviceRuntimeContract OneGen::PlaygroundAdapter OneGen::Producer
    Qt6::Core Qt6::Concurrent)

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
      ICON "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/icon.png"
      RUNTIME_FILES ${ONEGEN_GENICAM_RUNTIME_FILES})
  endif()
  message(STATUS "[OneGen] OneGenPlugin enabled.")
  if(ONEGEN_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(test_plugin_discovery Tests/test_plugin_discovery.cpp)
    set_target_properties(test_plugin_discovery PROPERTIES AUTOMOC ON)
    playground_sdk_add_runtime_contract(test_plugin_discovery)
    target_link_libraries(test_plugin_discovery PRIVATE Playground::DeviceRuntimeContract Qt6::Test ${CMAKE_DL_LIBS})
    target_compile_definitions(test_plugin_discovery PRIVATE
      ONEGEN_TEST_PLUGIN="$<TARGET_FILE:OneGenPlugin>"
      ONEGEN_TEST_PRODUCER="$<TARGET_FILE:SyntheticGenTL>")
    add_dependencies(test_plugin_discovery OneGenPlugin SyntheticGenTL)
    add_test(NAME test_plugin_discovery COMMAND test_plugin_discovery)
    set_tests_properties(test_plugin_discovery PROPERTIES TIMEOUT 30
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
    qt_add_executable(onegen_source_controller_test
      Utility/PlaygroundAdapter/Tests/SourceControllerTest.cpp
      Utility/PlaygroundAdapter/Source/OneGenSourceController.cpp
      Utility/PlaygroundAdapter/Source/OneGenParameterControl.cpp
      Utility/PlaygroundAdapter/Source/OneGenTools.cpp
      Utility/PlaygroundAdapter/Source/OneGenSourceController.h)
    playground_sdk_add_runtime_contract(onegen_source_controller_test)
    set_target_properties(onegen_source_controller_test PROPERTIES AUTOMOC ON)
    target_include_directories(onegen_source_controller_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source")
    target_link_libraries(onegen_source_controller_test PRIVATE
      Playground::DeviceRuntimeContract OneGen::PlaygroundAdapter OneGen::Producer
      Qt6::Concurrent Qt6::Test)
    target_compile_definitions(onegen_source_controller_test PRIVATE
      ONEGEN_TEST_PRODUCER="$<TARGET_FILE:SyntheticGenTL>")
    add_dependencies(onegen_source_controller_test SyntheticGenTL)
    add_test(NAME onegen_source_controller_test COMMAND onegen_source_controller_test -o -,txt)
    set_tests_properties(onegen_source_controller_test PROPERTIES TIMEOUT 300
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
    foreach(_details IN ITEMS on off)
      if(_details STREQUAL "on")
        set(_details_rule true)
      else()
        set(_details_rule false)
      endif()
      add_test(NAME onegen_source_controller_details_${_details}
        COMMAND onegen_source_controller_test firstInputDetailsFollowCategory -o -,txt)
      set_tests_properties(onegen_source_controller_details_${_details} PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ONEGEN_TEST_ACQUISITION_DETAILS=${_details};QT_LOGGING_RULES=playground.device.acquisition.details.debug=${_details_rule}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
    endforeach()
    add_test(NAME onegen_source_controller_unwritable_test COMMAND onegen_source_controller_test -o -,txt)
    set_tests_properties(onegen_source_controller_unwritable_test PROPERTIES TIMEOUT 300
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ONEGEN_SYNTHETIC_SCENARIO=M15;ONEGEN_TEST_ACQ_MODE=unwritable"
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_onegen_core_runtime_dir}")
  endif()
endif()
