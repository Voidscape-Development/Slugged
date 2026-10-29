# Unit tests for the parts of Slugged that do not need OBS: the document model,
# markup, tokens, the text pipeline and CPU-side geometry.
#
# This is a standalone project rather than part of the plugin build, because the
# plugin's CMake setup requires libobs and the OBS build dependencies. Configure
# it directly:
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

cmake_minimum_required(VERSION 3.22)

project(slugged-tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SLUGGED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SLUGGED_DEPS_DIR "${SLUGGED_ROOT}/deps")

if(NOT EXISTS "${SLUGGED_DEPS_DIR}/slughorn/CMakeLists.txt")
  message(FATAL_ERROR "Submodules are missing. Run:\n  git submodule update --init --recursive")
endif()

find_package(Freetype REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory("${SLUGGED_DEPS_DIR}/harfbuzz" harfbuzz EXCLUDE_FROM_ALL SYSTEM)

set(SHEENBIDI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SHEENBIDI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SLUGGED_DEPS_DIR}/sheenbidi" sheenbidi EXCLUDE_FROM_ALL SYSTEM)

set(SLUGHORN_FREETYPE ON CACHE BOOL "" FORCE)
add_subdirectory("${SLUGGED_DEPS_DIR}/slughorn" slughorn EXCLUDE_FROM_ALL SYSTEM)

add_executable(
  slugged-tests
  main.cpp
  test_core.cpp
  test_text.cpp
  test_render.cpp
  "${SLUGGED_ROOT}/src/util/log.cpp"
  "${SLUGGED_ROOT}/src/core/document.cpp"
  "${SLUGGED_ROOT}/src/core/markup.cpp"
  "${SLUGGED_ROOT}/src/core/text_feed.cpp"
  "${SLUGGED_ROOT}/src/core/tokens.cpp"
  "${SLUGGED_ROOT}/src/core/variables.cpp"
  "${SLUGGED_ROOT}/src/text/font_enum.cpp"
  "${SLUGGED_ROOT}/src/text/font_manager.cpp"
  "${SLUGGED_ROOT}/src/text/layout.cpp"
  "${SLUGGED_ROOT}/src/text/line_break.cpp"
  "${SLUGGED_ROOT}/src/text/shaper.cpp"
  "${SLUGGED_ROOT}/src/render/atlas_cache.cpp"
  "${SLUGGED_ROOT}/src/render/frame.cpp"
  "${SLUGGED_ROOT}/src/render/geometry.cpp"
  "${SLUGGED_ROOT}/src/render/rebuild_worker.cpp"
  "${SLUGGED_ROOT}/src/render/shape_cache.cpp"
)

target_include_directories(slugged-tests PRIVATE "${SLUGGED_ROOT}/src" "${SLUGGED_DEPS_DIR}/sheenbidi/Headers")
find_package(Threads REQUIRED)
target_link_libraries(
  slugged-tests
  PRIVATE slughorn harfbuzz SheenBidi Freetype::Freetype PkgConfig::FONTCONFIG Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(slugged-tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME slugged-tests COMMAND slugged-tests)
