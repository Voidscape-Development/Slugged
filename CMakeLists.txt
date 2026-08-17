cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# The editor window needs both Qt and the frontend API (to parent itself to the
# main window). A build without them still produces a fully working source that
# is configured from the standard properties dialog -- see
# src/obs/editor_bridge_stub.cpp.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Gui REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Gui)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# --------------------------------------------------------------------------
# Vendored dependencies
#
# slughorn, HarfBuzz and SheenBidi are built from source as submodules rather
# than discovered on the system. Every one of the three platforms CI builds for
# would otherwise need its own package story, and slughorn in particular has no
# distribution packages at all. FreeType is the exception: OBS already ships it
# in obs-deps on Windows and macOS, so the system copy is preferred and only
# fetched when missing.
# --------------------------------------------------------------------------

include(FetchContent)

set(SLUGGED_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps")

if(NOT EXISTS "${SLUGGED_DEPS_DIR}/slughorn/CMakeLists.txt")
  message(FATAL_ERROR "Submodules are missing. Run:\n  git submodule update --init --recursive")
endif()

find_package(Freetype REQUIRED)

# ---- HarfBuzz ----
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

add_subdirectory("${SLUGGED_DEPS_DIR}/harfbuzz" EXCLUDE_FROM_ALL SYSTEM)

# ---- SheenBidi ----
set(SHEENBIDI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SHEENBIDI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

add_subdirectory("${SLUGGED_DEPS_DIR}/sheenbidi" EXCLUDE_FROM_ALL SYSTEM)

# ---- slughorn ----
set(SLUGHORN_FREETYPE ON CACHE BOOL "" FORCE)

add_subdirectory("${SLUGGED_DEPS_DIR}/slughorn" EXCLUDE_FROM_ALL SYSTEM)

# The vendored dependencies are compiled with this project's warning flags,
# because OBS's compilerconfig applies them at directory scope before these
# subdirectories are added. Combined with CMAKE_COMPILE_WARNING_AS_ERROR (set by
# the CMake presets), an unused parameter in someone else's library fails our
# build.
#
# COMPILE_WARNING_AS_ERROR is the right lever rather than appending -Wno-error:
# CMake emits its own -Werror *after* all target options, so a target-level
# -Wno-error is silently overridden. Our own sources keep warnings as errors.
#
# The SYSTEM keyword above covers the other half of the problem -- warnings
# raised inside these libraries' *headers*, which would otherwise fail the
# translation units of ours that include them.
foreach(_slugged_dep IN ITEMS slughorn harfbuzz SheenBidi)
  if(TARGET ${_slugged_dep})
    set_target_properties(${_slugged_dep} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  endif()
endforeach()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE slughorn harfbuzz SheenBidi Freetype::Freetype)

# fontconfig is the font-enumeration backend on Linux only; Windows uses
# DirectWrite and macOS uses CoreText, both of which ship with the OS.
if(OS_LINUX OR OS_FREEBSD)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::FONTCONFIG)
endif()

if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE dwrite)
endif()

if(OS_MACOS)
  find_library(CORE_TEXT CoreText REQUIRED)
  find_library(CORE_FOUNDATION CoreFoundation REQUIRED)

  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CORE_TEXT} ${CORE_FOUNDATION})
endif()

target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SLUGGED_DEPS_DIR}/sheenbidi/Headers"
)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/util/log.cpp
    src/util/log.hpp
    src/core/document.cpp
    src/core/document.hpp
    src/core/text_feed.cpp
    src/core/text_feed.hpp
    src/core/tokens.cpp
    src/core/tokens.hpp
    src/core/types.hpp
    src/text/font_enum.cpp
    src/text/font_enum.hpp
    src/text/font_manager.cpp
    src/text/font_manager.hpp
    src/text/layout.cpp
    src/text/layout.hpp
    src/text/shaper.cpp
    src/text/shaper.hpp
    src/render/atlas_cache.cpp
    src/render/atlas_cache.hpp
    src/render/geometry.cpp
    src/render/geometry.hpp
    src/render/renderer.cpp
    src/render/renderer.hpp
    src/obs/editor_bridge.hpp
    src/obs/migrate.cpp
    src/obs/migrate.hpp
    src/obs/settings.cpp
    src/obs/settings.hpp
    src/obs/slugged_source.cpp
    src/obs/slugged_source.hpp
)

if(ENABLE_QT)
  # Headers are listed so AUTOMOC sees the Q_OBJECT classes.
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/ui/color_button.cpp
      src/ui/color_button.hpp
      src/ui/editor_window.cpp
      src/ui/editor_window.hpp
      src/ui/preview_widget.cpp
      src/ui/preview_widget.hpp
  )
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/obs/editor_bridge_stub.cpp)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
