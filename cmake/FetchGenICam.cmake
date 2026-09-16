# Official EMVA GenICam 2026.07 only (new CI layout).
# Nested per-platform archives: GenApi-3.5.1-<platform>-SDK + Runtime.
# No vendor SDKs. CTI producers are loadCti() at runtime.
#
# Override: -DONEGEN_GENICAM_ROOT=...  (extracted SDK root with include/GenApi)
#           -DONEGEN_GENICAM_RUNTIME_ROOT=... (extracted runtime root)
#           -DONEGEN_GENTL_INCLUDE=... (directory containing GenTL/GenTL.h)
#           -DONEGEN_GENICAM_URL=...

set(ONEGEN_GENICAM_VERSION "2026.07")
set(ONEGEN_GENICAM_SHA256 "e74b8d982c3e7697399b03c254b6e8c596eddf0279959eba7de18734e279b64f")

set(_cache "${CMAKE_CURRENT_LIST_DIR}/../genicam")
set(_zip "${_cache}/GenICam_Package_${ONEGEN_GENICAM_VERSION}.zip")
set(_zip_tmp "${_zip}.tmp")
set(_pkg "${_cache}/pkg")
file(MAKE_DIRECTORY "${_cache}")

if(NOT DEFINED ONEGEN_GENICAM_URL)
  set(ONEGEN_GENICAM_URL
    "https://www.emva.org/wp-content/uploads/GenICam_Package_${ONEGEN_GENICAM_VERSION}.zip")
endif()

if(WIN32)
  set(_plat "Win64_x64")
  set(_sdk_glob "GenApi-*-Win64_x64-SDK.zip")
  set(_rt_glob "GenApi-*-Win64_x64-Runtime.zip")
elseif(APPLE)
  set(_plat "Mac64_Universal")
  set(_sdk_glob "GenApi-*-Mac64_Universal-SDK.tar.gz")
  set(_rt_glob "GenApi-*-Mac64_Universal-Runtime.tar.gz")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
  set(_plat "Linux64_ARM")
  set(_sdk_glob "GenApi-*-Linux64_ARM-SDK.tar.gz")
  set(_rt_glob "GenApi-*-Linux64_ARM-Runtime.tar.gz")
else()
  set(_plat "Linux64_x64")
  set(_sdk_glob "GenApi-*-Linux64_x64-SDK.tar.gz")
  set(_rt_glob "GenApi-*-Linux64_x64-Runtime.tar.gz")
endif()

function(_og_extract archive dest)
  file(MAKE_DIRECTORY "${dest}")
  get_filename_component(_ext "${archive}" LAST_EXT)
  get_filename_component(_name "${archive}" NAME)
  if(_name MATCHES "\\.tar\\.gz$" OR _name MATCHES "\\.tgz$")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${archive}"
      WORKING_DIRECTORY "${dest}" COMMAND_ERROR_IS_FATAL ANY)
  else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${archive}"
      WORKING_DIRECTORY "${dest}" COMMAND_ERROR_IS_FATAL ANY)
  endif()
endfunction()

function(_og_find_genapi_include dir out)
  if(EXISTS "${dir}/include/GenApi/GenApi.h")
    set(${out} "${dir}" PARENT_SCOPE)
    return()
  endif()
  if(EXISTS "${dir}/library/CPP/include/GenApi/GenApi.h")
    set(${out} "${dir}/library/CPP" PARENT_SCOPE)
    return()
  endif()
  file(GLOB_RECURSE _hits "${dir}/**/GenApi.h")
  foreach(_h ${_hits})
    get_filename_component(_d "${_h}" DIRECTORY) # .../GenApi
    get_filename_component(_inc "${_d}" DIRECTORY) # .../include
    if(EXISTS "${_inc}/GenApi/GenApi.h")
      get_filename_component(_root "${_inc}" DIRECTORY)
      set(${out} "${_root}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} "" PARENT_SCOPE)
endfunction()

if(ONEGEN_GENICAM_ROOT)
  _og_find_genapi_include("${ONEGEN_GENICAM_ROOT}" _sdk_root)
  if(NOT _sdk_root)
    message(FATAL_ERROR "ONEGEN_GENICAM_ROOT has no GenApi.h")
  endif()
  if(ONEGEN_GENICAM_RUNTIME_ROOT)
    set(_rt_extract "${ONEGEN_GENICAM_RUNTIME_ROOT}")
  endif()
else()
  if(EXISTS "${_zip}")
    file(SHA256 "${_zip}" _zip_hash)
    if(NOT "${_zip_hash}" STREQUAL "${ONEGEN_GENICAM_SHA256}")
      file(REMOVE "${_zip}")
    endif()
  endif()
  if(NOT EXISTS "${_zip}")
    message(STATUS "Downloading GenICam ${ONEGEN_GENICAM_VERSION}")
    file(DOWNLOAD "${ONEGEN_GENICAM_URL}" "${_zip_tmp}"
      EXPECTED_HASH SHA256=${ONEGEN_GENICAM_SHA256}
      SHOW_PROGRESS
      STATUS _st TIMEOUT 300 TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _st 1 _msg)
      file(REMOVE "${_zip_tmp}")
      message(FATAL_ERROR "GenICam download failed: ${_msg}")
    endif()
    file(RENAME "${_zip_tmp}" "${_zip}")
  endif()

  find_program(UNZIP_EXECUTABLE unzip)
  file(MAKE_DIRECTORY "${_pkg}")
  if(UNZIP_EXECUTABLE)
    message(STATUS "Extracting GenApi ${_plat} + GenTL.h from EMVA zip")
    execute_process(
      COMMAND ${UNZIP_EXECUTABLE} -o -q "${_zip}"
        "*${_plat}*"
        "*GenTL*"
        "*PFNC*"
      WORKING_DIRECTORY "${_pkg}"
      RESULT_VARIABLE _uz)
    if(NOT _uz EQUAL 0)
      execute_process(COMMAND ${UNZIP_EXECUTABLE} -o -q "${_zip}" -d "${_pkg}"
        COMMAND_ERROR_IS_FATAL ANY)
    endif()
  elseif(NOT EXISTS "${_pkg}/GenTL/GenTL.h")
    _og_extract("${_zip}" "${_pkg}")
  endif()

  file(GLOB_RECURSE _sdk_arch
    "${_pkg}/*${_plat}-SDK.tar.gz"
    "${_pkg}/*${_plat}-SDK.zip")
  if(NOT _sdk_arch)
    message(FATAL_ERROR "No ${_sdk_glob} in EMVA package. Host platform=${_plat}.")
  endif()
  list(GET _sdk_arch 0 _sdk_arch)

  set(_sdk_extract "${_cache}/sdk-${_plat}")
  _og_find_genapi_include("${_sdk_extract}" _sdk_root)
  if(NOT _sdk_root)
    _og_extract("${_sdk_arch}" "${_sdk_extract}")
    _og_find_genapi_include("${_sdk_extract}" _sdk_root)
  endif()
  if(NOT _sdk_root)
    message(FATAL_ERROR "GenApi SDK extracted but GenApi.h not found in ${_sdk_extract}")
  endif()

  file(GLOB_RECURSE _rt_arch "${_pkg}/${_rt_glob}")
  set(_rt_extract "${_cache}/runtime-${_plat}")
  if(_rt_arch)
    list(GET _rt_arch 0 _rt_arch)
    if(NOT EXISTS "${_rt_extract}")
      _og_extract("${_rt_arch}" "${_rt_extract}")
    endif()
  endif()
endif()

set(_inc "${_sdk_root}/include")
if(NOT EXISTS "${_inc}/GenApi/GenApi.h")
  set(_inc "${_sdk_root}/library/CPP/include")
endif()
if(NOT EXISTS "${_inc}/GenApi/GenApi.h")
  message(FATAL_ERROR "GenApi.h missing under ${_sdk_root}")
endif()

# GenTL.h lives in the meta package (spec header).
set(_gentl_inc "")
if(ONEGEN_GENTL_INCLUDE)
  if(NOT EXISTS "${ONEGEN_GENTL_INCLUDE}/GenTL/GenTL.h")
    message(FATAL_ERROR "ONEGEN_GENTL_INCLUDE has no GenTL/GenTL.h")
  endif()
  set(_gentl_inc "${ONEGEN_GENTL_INCLUDE}")
elseif(ONEGEN_GENICAM_ROOT AND EXISTS "${ONEGEN_GENICAM_ROOT}/include/GenTL/GenTL.h")
  set(_gentl_inc "${ONEGEN_GENICAM_ROOT}/include")
elseif(EXISTS "${_pkg}/GenTL/GenTL.h")
  set(_gentl_inc "${_pkg}/GenTL")
  # so #include <GenTL/GenTL.h> works
  file(MAKE_DIRECTORY "${_cache}/include/GenTL")
  configure_file("${_pkg}/GenTL/GenTL.h" "${_cache}/include/GenTL/GenTL.h" COPYONLY)
  set(_gentl_inc "${_cache}/include")
endif()
if(NOT _gentl_inc)
  message(FATAL_ERROR "GenTL/GenTL.h missing; set ONEGEN_GENTL_INCLUDE")
endif()

set(_libdirs
  "${_sdk_root}/lib"
  "${_sdk_root}/lib/${_plat}"
  "${_sdk_root}/bin"
  "${_sdk_root}/bin/${_plat}"
  "${_sdk_root}/lib64"
)
if(DEFINED _rt_extract)
  list(APPEND _libdirs
    "${_rt_extract}"
    "${_rt_extract}/lib" "${_rt_extract}/lib/${_plat}"
    "${_rt_extract}/bin" "${_rt_extract}/bin/${_plat}")
endif()

set(_genapi_libs)
set(_rpath)
foreach(_d ${_libdirs})
  if(NOT EXISTS "${_d}")
    continue()
  endif()
  if(WIN32)
    file(GLOB _hit "${_d}/GenApi*.lib" "${_d}/GCBase*.lib")
  else()
    file(GLOB _hit
      "${_d}/libGenApi*"
      "${_d}/libGCBase*"
      "${_d}/libLog*"
      "${_d}/libMathParser*"
      "${_d}/libNodeMapData*"
      "${_d}/libXmlParser*"
      "${_d}/liblog4cpp*")
  endif()
  if(_hit)
    list(APPEND _genapi_libs ${_hit})
    set(_rpath "${_d}")
  endif()
endforeach()

if(NOT _genapi_libs)
  set(_search_roots "${_sdk_root}")
  if(DEFINED _rt_extract)
    list(APPEND _search_roots "${_rt_extract}")
  endif()
  if(APPLE)
    foreach(_root ${_search_roots})
      file(GLOB_RECURSE _found "${_root}/*.dylib")
      list(APPEND _dylibs ${_found})
    endforeach()
  elseif(UNIX)
    foreach(_root ${_search_roots})
      file(GLOB_RECURSE _found "${_root}/*.so*")
      list(APPEND _dylibs ${_found})
    endforeach()
  endif()
  foreach(_f ${_dylibs})
    get_filename_component(_n "${_f}" NAME)
    if(_n MATCHES "GenApi|GCBase|libLog|MathParser|NodeMapData|XmlParser|log4cpp")
      list(APPEND _genapi_libs "${_f}")
      get_filename_component(_rpath "${_f}" DIRECTORY)
    endif()
  endforeach()
endif()

set(ONEGEN_GENICAM_RUNTIME_FILES)
if(DEFINED _rt_extract)
  if(WIN32)
    file(GLOB_RECURSE ONEGEN_GENICAM_RUNTIME_FILES "${_rt_extract}/*.dll")
  elseif(APPLE)
    file(GLOB_RECURSE ONEGEN_GENICAM_RUNTIME_FILES "${_rt_extract}/*.dylib")
  elseif(UNIX)
    file(GLOB_RECURSE ONEGEN_GENICAM_RUNTIME_FILES "${_rt_extract}/*.so*")
  endif()
endif()

if(NOT _genapi_libs)
  message(FATAL_ERROR "No GenApi libraries for ${_plat} under ${_sdk_root}")
endif()

add_library(genicam::genicam INTERFACE IMPORTED)
target_include_directories(genicam::genicam INTERFACE "${_inc}")
if(_gentl_inc)
  target_include_directories(genicam::genicam INTERFACE "${_gentl_inc}")
endif()
target_link_libraries(genicam::genicam INTERFACE ${_genapi_libs})
if(_rpath AND NOT WIN32)
  target_link_options(genicam::genicam INTERFACE "LINKER:-rpath,${_rpath}")
endif()

message(STATUS "GenApi include: ${_inc}")
message(STATUS "GenApi libs: ${_genapi_libs}")
