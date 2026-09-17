include("${CMAKE_CURRENT_LIST_DIR}/../Utility/PlaygroundAdapter/Package/Package.cmake")
get_filename_component(worker_name "${WORKER_FILE}" NAME)
if(NOT EXISTS "${WORKER_FILE}" OR
   NOT PLAYGROUND_PLUGIN_ENVIRONMENT_FILES STREQUAL "ONEGEN_WORKER_PATH=${worker_name}")
  message(FATAL_ERROR "OneGen package worker binding does not match the built executable: ${WORKER_FILE}")
endif()
