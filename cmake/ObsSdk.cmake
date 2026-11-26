# Provides OBS::libobs and OBS::frontend-api without building OBS from source.
#
# Headers come from an obs-studio source checkout (OBS_SOURCE_DIR) that should
# match the installed OBS version. Import libraries are generated from the
# DLLs of the installed OBS (OBS_INSTALL_DIR) with `link /dump /exports` and
# `lib /def`, which is all MSVC needs to link against a C API.

set(_obs_libobs_inc "${OBS_SOURCE_DIR}/libobs")
set(_obs_frontend_inc "${OBS_SOURCE_DIR}/frontend/api")
set(_obs_bin "${OBS_INSTALL_DIR}/bin/64bit")

if(NOT EXISTS "${_obs_libobs_inc}/obs-module.h" OR NOT EXISTS "${_obs_frontend_inc}/obs-frontend-api.h")
  message(FATAL_ERROR
    "OBS headers not found under OBS_SOURCE_DIR='${OBS_SOURCE_DIR}'.\n"
    "Run scripts/fetch-obs-headers.ps1, or point OBS_SOURCE_DIR at an obs-studio checkout "
    "(31.0 or newer, where the frontend API lives in frontend/api).")
endif()
foreach(_dll obs.dll obs-frontend-api.dll)
  if(NOT EXISTS "${_obs_bin}/${_dll}")
    message(FATAL_ERROR "'${_obs_bin}/${_dll}' not found. Set OBS_INSTALL_DIR to your OBS Studio install folder.")
  endif()
endforeach()

# libobs normally generates obsconfig.h at build time; only these two values are used by public headers.
set(_obs_gen "${CMAKE_BINARY_DIR}/obs-sdk")
file(MAKE_DIRECTORY "${_obs_gen}/include")
file(WRITE "${_obs_gen}/include/obsconfig.h.tmp"
  "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
file(COPY_FILE "${_obs_gen}/include/obsconfig.h.tmp" "${_obs_gen}/include/obsconfig.h" ONLY_IF_DIFFERENT)

get_filename_component(_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
find_program(OBS_SDK_LIB_TOOL NAMES lib.exe HINTS "${_msvc_bin}" REQUIRED)

function(_obs_make_import_lib target dll_name)
  set(_dll "${_obs_bin}/${dll_name}.dll")
  set(_def "${_obs_gen}/${dll_name}.def")
  set(_lib "${_obs_gen}/${dll_name}.lib")

  execute_process(
    COMMAND "${CMAKE_LINKER}" /dump /nologo /exports "${_dll}"
    OUTPUT_VARIABLE _dump
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Failed to read exports of ${_dll}")
  endif()

  # Export rows look like: "   ordinal hint RVA      name"
  string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[A-Za-z_][A-Za-z0-9_]*" _rows "${_dump}")
  set(_def_text "LIBRARY \"${dll_name}.dll\"\nEXPORTS\n")
  set(_count 0)
  foreach(_row IN LISTS _rows)
    string(REGEX REPLACE ".* ([A-Za-z_][A-Za-z0-9_]*)$" "\\1" _sym "${_row}")
    string(APPEND _def_text "  ${_sym}\n")
    math(EXPR _count "${_count} + 1")
  endforeach()
  if(_count LESS 10)
    message(FATAL_ERROR "Only found ${_count} exports in ${_dll}; refusing to continue.")
  endif()

  file(WRITE "${_def}.tmp" "${_def_text}")
  file(COPY_FILE "${_def}.tmp" "${_def}" ONLY_IF_DIFFERENT)
  if(NOT EXISTS "${_lib}" OR "${_def}" IS_NEWER_THAN "${_lib}")
    execute_process(
      COMMAND "${OBS_SDK_LIB_TOOL}" /nologo /machine:x64 "/def:${_def}" "/out:${_lib}"
      RESULT_VARIABLE _rc
      OUTPUT_VARIABLE _out
      ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "lib.exe failed for ${dll_name}:\n${_out}")
    endif()
  endif()
  message(STATUS "OBS SDK: ${dll_name}.lib generated from ${_count} exports")

  add_library(${target} SHARED IMPORTED GLOBAL)
  set_target_properties(${target} PROPERTIES
    IMPORTED_IMPLIB "${_lib}"
    IMPORTED_LOCATION "${_dll}")
endfunction()

_obs_make_import_lib(OBS::libobs obs)
_obs_make_import_lib(OBS::frontend-api obs-frontend-api)

set_target_properties(OBS::libobs PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_obs_libobs_inc};${_obs_gen}/include")
set_target_properties(OBS::frontend-api PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_obs_frontend_inc}"
  INTERFACE_LINK_LIBRARIES OBS::libobs)

# Report which OBS this is being built against.
file(STRINGS "${_obs_libobs_inc}/obs-config.h" _ver_lines REGEX "#define LIBOBS_API_(MAJOR|MINOR|PATCH)_VER [0-9]+")
foreach(_l IN LISTS _ver_lines)
  string(REGEX REPLACE ".*_(MAJOR|MINOR|PATCH)_VER ([0-9]+)" "\\1;\\2" _kv "${_l}")
  list(GET _kv 0 _k)
  list(GET _kv 1 _v)
  set(_obs_${_k} ${_v})
endforeach()
message(STATUS "OBS SDK: headers are libobs ${_obs_MAJOR}.${_obs_MINOR}.${_obs_PATCH}")
