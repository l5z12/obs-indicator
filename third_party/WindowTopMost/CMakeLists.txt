# Vendored build of WindowTopMost (https://github.com/xmc0211/WindowTopMost), MIT-licensed.
# Produces two DLLs used only by the optional "show over fullscreen games" feature:
#   WindowTopMost.dll - the API we call (WTMInit / WTMSetWindowBand / WTMUninit)
#   IAMKeyHacker.dll  - the worker WindowTopMost injects into Explorer.exe at runtime
# Only the source files the upstream .vcxproj actually compiles are vendored here; the
# unused Detours copy under IAMKeyHacker/src was left out. See LICENSE and mHook_COPYING.

set(WTM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# Third-party C/C++ that does not meet our /W4 /permissive- bar; build it quietly and
# permissively so our strict flags do not leak in. Static CRT is inherited from the parent
# (CMAKE_MSVC_RUNTIME_LIBRARY), so neither DLL depends on a vcruntime redistributable.
# Note: WIN32_LEAN_AND_MEAN / NOMINMAX are defined by the upstream headers themselves, so we
# do not predefine them here (that only causes C4005 redefinition warnings).
set(WTM_DEFS UNICODE _UNICODE WIN32 _WINDOWS _USRDLL
             _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
# /wd5287: the vendored disassembler mixes 16- and 32-bit x86 register enums on purpose.
set(WTM_OPTS /W1 /utf-8 /wd5287)

# --- IAMKeyHacker.dll: the injected worker (DllMain does the work; no exports needed) ---
add_library(IAMKeyHacker MODULE
  "${WTM_ROOT}/IAMKeyHacker/DllMain.cpp"
  "${WTM_ROOT}/IAMKeyHacker/IAMKeyBlock.cpp"
  "${WTM_ROOT}/Common Headers/ThreadStopHandler.cpp"
  "${WTM_ROOT}/IAMKeyHacker/mhook-lib/mhook.c"
  "${WTM_ROOT}/IAMKeyHacker/disasm-lib/cpu.c"
  "${WTM_ROOT}/IAMKeyHacker/disasm-lib/disasm.c"
  "${WTM_ROOT}/IAMKeyHacker/disasm-lib/disasm_x86.c")
set_target_properties(IAMKeyHacker PROPERTIES OUTPUT_NAME "IAMKeyHacker" PREFIX ""
  COMPILE_WARNING_AS_ERROR OFF)
target_include_directories(IAMKeyHacker PRIVATE "${WTM_ROOT}" "${WTM_ROOT}/IAMKeyHacker")
target_compile_definitions(IAMKeyHacker PRIVATE ${WTM_DEFS} SOURCE_EXPORTS)
target_compile_options(IAMKeyHacker PRIVATE ${WTM_OPTS})
target_link_libraries(IAMKeyHacker PRIVATE user32 advapi32)

# --- WindowTopMost.dll: the API surface we load at runtime (exports via the .def) ---
add_library(WindowTopMost MODULE
  "${WTM_ROOT}/WindowTopMost/DllMain.cpp"
  "${WTM_ROOT}/WindowTopMost/Convert.cpp"
  "${WTM_ROOT}/WindowTopMost/UIAccess.c"
  "${WTM_ROOT}/Common Headers/ThreadStopHandler.cpp")
set_target_properties(WindowTopMost PROPERTIES
  OUTPUT_NAME "WindowTopMost" PREFIX ""
  COMPILE_WARNING_AS_ERROR OFF
  LINK_FLAGS "/DEF:\"${WTM_ROOT}/WindowTopMost/WindowTopMost.def\"")
target_include_directories(WindowTopMost PRIVATE "${WTM_ROOT}" "${WTM_ROOT}/WindowTopMost")
target_compile_definitions(WindowTopMost PRIVATE ${WTM_DEFS} WINDOWTOPMOST_EXPORTS)
target_compile_options(WindowTopMost PRIVATE ${WTM_OPTS})
target_link_libraries(WindowTopMost PRIVATE user32 advapi32 shell32)

# --- mhook as a static library, for obs-indicator-hook.dll (drawing inside OpenGL games) ---
add_library(mhook STATIC
  "${WTM_ROOT}/IAMKeyHacker/mhook-lib/mhook.c"
  "${WTM_ROOT}/IAMKeyHacker/disasm-lib/cpu.c"
  "${WTM_ROOT}/IAMKeyHacker/disasm-lib/disasm.c"
  "${WTM_ROOT}/IAMKeyHacker/disasm-lib/disasm_x86.c")
set_target_properties(mhook PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
target_include_directories(mhook PUBLIC "${WTM_ROOT}/IAMKeyHacker")
target_compile_definitions(mhook PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(mhook PRIVATE ${WTM_OPTS})

# WindowTopMost.dll injects IAMKeyHacker.dll by resolving it next to itself. Both MODULE
# targets build into the same directory here, so they are already siblings; the install step
# in the top-level CMakeLists packages the pair next to the plugin.
add_dependencies(WindowTopMost IAMKeyHacker)
