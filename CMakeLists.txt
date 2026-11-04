cmake_minimum_required(VERSION 3.21)
# 0.N.M: N is the stage in docs/ROADMAP.md, M the delivery within it.
project(mdmi_plugin VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(MDMI_BUILD_LABEL "unknown" CACHE STRING "Revision shown as part of the plugin version")
set(MDMI_VERSION "${PROJECT_VERSION}+${MDMI_BUILD_LABEL}")
file(WRITE "${CMAKE_BINARY_DIR}/label.txt" "${MDMI_VERSION}\n")

set(MDMI_ENGINE "${CMAKE_SOURCE_DIR}/build/engine/libblastem.dll")
set(MDMI_ROM "${CMAKE_SOURCE_DIR}/build/rom/mdmi-everdrive.bin")
set(BLASTEM_DIR "${CMAKE_SOURCE_DIR}/third_party/blastem")

set(CLAP_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/clap")
set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/vst3sdk")
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE)
set(CLAP_WRAPPER_OUTPUT_NAME "MDMI Mega Drive")
add_subdirectory(third_party/clap-wrapper EXCLUDE_FROM_ALL)

add_library(mdmi_impl STATIC src/plugin.c src/engine.c src/patch.c src/instrument.c src/state.c src/files.c src/resampler.c src/embed.c src/gui.c)
target_compile_options(mdmi_impl PRIVATE -Wall -Wextra -Wno-cast-function-type)
target_include_directories(mdmi_impl PUBLIC "${CLAP_SDK_ROOT}/include" PRIVATE "${BLASTEM_DIR}")
target_link_libraries(mdmi_impl PUBLIC m avrt user32 gdi32 ole32 shell32 comdlg32 uuid)
target_link_options(mdmi_impl INTERFACE -static -s)
set_source_files_properties(src/plugin.c PROPERTIES
  COMPILE_DEFINITIONS "MDMI_VERSION=\"${MDMI_VERSION}\"")
set_source_files_properties(src/embed.c PROPERTIES
  COMPILE_DEFINITIONS "MDMI_ENGINE_PATH=${MDMI_ENGINE};MDMI_ROM_PATH=${MDMI_ROM}"
  OBJECT_DEPENDS "${MDMI_ENGINE};${MDMI_ROM}")

make_clapfirst_plugins(
  TARGET_NAME mdmi
  IMPL_TARGET mdmi_impl
  OUTPUT_NAME "MDMI Mega Drive"
  ENTRY_SOURCE src/clap_entry.cpp
  BUNDLE_IDENTIFIER "dev.mdmi-plugin.mega-drive"
  BUNDLE_VERSION "${PROJECT_VERSION}"
  ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
  PLUGIN_FORMATS CLAP VST3
)

add_executable(clap_host tools/test/clap_host.c)
target_compile_options(clap_host PRIVATE -Wall -Wextra -Wno-cast-function-type)
target_include_directories(clap_host PRIVATE "${CLAP_SDK_ROOT}/include")
target_link_options(clap_host PRIVATE -static)
target_link_libraries(clap_host PRIVATE avrt)
add_dependencies(mdmi_all clap_host)

set_target_properties(mdmi_vst3 PROPERTIES PREFIX "")

add_executable(gui_test tools/test/gui_test.c)
target_compile_options(gui_test PRIVATE -Wall -Wextra -Wno-cast-function-type)
target_include_directories(gui_test PRIVATE "${CLAP_SDK_ROOT}/include")
target_link_options(gui_test PRIVATE -static)
target_link_libraries(gui_test PRIVATE gdi32 user32)
add_dependencies(mdmi_all gui_test)

add_executable(vst3_host tools/test/vst3_host.cpp)
target_include_directories(vst3_host PRIVATE "${VST3_SDK_ROOT}")
target_link_options(vst3_host PRIVATE -static)
target_sources(vst3_host PRIVATE "${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp")
target_link_libraries(vst3_host PRIVATE ole32 user32)
add_dependencies(mdmi_all vst3_host)

add_executable(reg_test tools/test/reg_test.c)
target_compile_options(reg_test PRIVATE -Wall -Wextra -Wno-cast-function-type)
target_include_directories(reg_test PRIVATE src "${CLAP_SDK_ROOT}/include")
target_link_libraries(reg_test PRIVATE mdmi_impl)
add_dependencies(mdmi_all reg_test)

add_executable(patch_test tools/test/patch_test.c)
target_compile_options(patch_test PRIVATE -Wall -Wextra -Wno-cast-function-type)
target_include_directories(patch_test PRIVATE src "${CLAP_SDK_ROOT}/include")
target_link_libraries(patch_test PRIVATE mdmi_impl)
add_dependencies(mdmi_all patch_test)

add_executable(param_test tools/test/param_test.c)
target_compile_options(param_test PRIVATE -Wall -Wextra -Wno-cast-function-type)
target_include_directories(param_test PRIVATE "${CLAP_SDK_ROOT}/include")
target_link_options(param_test PRIVATE -static)
add_dependencies(mdmi_all param_test)
