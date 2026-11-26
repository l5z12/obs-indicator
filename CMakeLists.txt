cmake_minimum_required(VERSION 3.24)

# The version is the single source of truth for the DLL, manifest.json and release zips. Knope bumps it when
# releasing; keep "project(obs-indicator VERSION x.y.z" on one line so it can find it.
project(obs-indicator VERSION 1.0.0
  DESCRIPTION "ShadowPlay-style on-screen status indicator and notifications for OBS Studio"
  HOMEPAGE_URL "https://github.com/l5z12/obs-indicator"
  LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "obs-indicator is a Windows-only plugin.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "obs-indicator must be built for x64 (OBS Studio is 64-bit only).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: avoids depending on whichever msvcp140.dll OBS ships (mixing a
# newer toolset with an older runtime crashes in std::mutex).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(OBS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-studio"
    CACHE PATH "obs-studio source checkout (only libobs/ and frontend/api/ headers are used)")
set(OBS_INSTALL_DIR "$ENV{ProgramFiles}/obs-studio"
    CACHE PATH "Installed OBS Studio; obs.dll and obs-frontend-api.dll are read to create import libraries")
option(OBS_INDICATOR_BUILD_TOOLS "Build the standalone preview/demo tools" ON)
option(OBS_INDICATOR_WITH_FULLSCREEN_TOPMOST
  "Build and bundle WindowTopMost for the optional 'show over fullscreen games' feature" ON)

include(cmake/ObsSdk.cmake)

add_link_options($<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO> $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
                 $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)

set(COMMON_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C)
set(COMMON_OPTS /W4 /permissive- /utf-8 /Zc:__cplusplus /wd4100)

# Everything that does not need libobs lives here, so the tools can use it too.
add_library(overlay-core STATIC
  src/overlay.cpp
  src/overlay.hpp
  src/renderer.cpp
  src/renderer.hpp
  src/monitors.cpp
  src/monitors.hpp
  src/topmost.cpp
  src/topmost.hpp
  src/settings.hpp
  src/settings-dialog.cpp
  src/settings-dialog.hpp
  src/resource.h)
target_include_directories(overlay-core PUBLIC src)
target_compile_definitions(overlay-core PUBLIC ${COMMON_DEFS})
target_compile_options(overlay-core PRIVATE ${COMMON_OPTS})
target_link_libraries(overlay-core PUBLIC d2d1 dwrite shcore user32 gdi32 comctl32 comdlg32 uxtheme)

add_library(obs-indicator MODULE
  src/plugin-main.cpp
  src/obs-indicator.rc
  src/obs-indicator.manifest)
target_compile_definitions(obs-indicator PRIVATE
  PLUGIN_VERSION="${PROJECT_VERSION}"
  PLUGIN_VERSION_RC=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
target_compile_options(obs-indicator PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${COMMON_OPTS}>)
target_link_libraries(obs-indicator PRIVATE overlay-core OBS::libobs OBS::frontend-api)
set_target_properties(obs-indicator PROPERTIES PREFIX "")

# Optional: the WindowTopMost DLLs that let the overlay sit above fullscreen games. The plugin
# loads WindowTopMost.dll by full path at runtime (no link- or compile-time dependency), so if
# the DLLs are absent the feature simply stays unavailable.
if(OBS_INDICATOR_WITH_FULLSCREEN_TOPMOST)
  add_subdirectory(third_party/WindowTopMost)
  add_dependencies(obs-indicator WindowTopMost)
  # Stage the DLLs next to the plugin in the build tree so a plain `cmake --build` is runnable.
  add_custom_command(TARGET obs-indicator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WindowTopMost>" "$<TARGET_FILE:IAMKeyHacker>" "$<TARGET_FILE_DIR:obs-indicator>")
endif()

if(OBS_INDICATOR_BUILD_TOOLS)
  # Renders the indicator/notification artwork to a PNG (no window, no OBS).
  add_executable(overlay-preview tools/preview.cpp)
  target_compile_options(overlay-preview PRIVATE ${COMMON_OPTS})
  target_link_libraries(overlay-preview PRIVATE overlay-core windowscodecs ole32)

  # Drives the real overlay window with fake OBS events, and can open the
  # settings dialog, all without OBS running.
  add_executable(overlay-demo tools/demo.cpp src/obs-indicator.rc src/obs-indicator.manifest)
  target_compile_definitions(overlay-demo PRIVATE
    PLUGIN_VERSION="${PROJECT_VERSION}"
    PLUGIN_VERSION_RC=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
  target_compile_options(overlay-demo PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${COMMON_OPTS}>)
  target_link_libraries(overlay-demo PRIVATE overlay-core windowscodecs ole32 msimg32)
endif()

# OBS 33+ plugin layout (https://obsproject.com/kb/legacy-plugin-developer-info):
#   C:/ProgramData/obs-studio/plugins/obs-indicator/obs-indicator.dll
#   C:/ProgramData/obs-studio/plugins/obs-indicator/data/...
#   cmake --install build --prefix "C:/ProgramData/obs-studio/plugins/obs-indicator"
# The older bin/64bit sub-folder still loads but is flagged "Legacy" by OBS.
install(TARGETS obs-indicator LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(FILES $<TARGET_PDB_FILE:obs-indicator> DESTINATION . OPTIONAL)
install(DIRECTORY data/ DESTINATION data)

# Bundle the WindowTopMost DLLs beside the plugin, plus their MIT licenses (as required).
if(OBS_INDICATOR_WITH_FULLSCREEN_TOPMOST)
  install(TARGETS WindowTopMost IAMKeyHacker LIBRARY DESTINATION . RUNTIME DESTINATION .)
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WindowTopMost/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WindowTopMost/IAMKeyHacker/mHook_COPYING"
    DESTINATION licenses/WindowTopMost)
endif()

# Plugin metadata shown in OBS's plugin manager (Tools > Plugin Manager), read from data/manifest.json.
configure_file(cmake/manifest.json.in "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" DESTINATION data)
