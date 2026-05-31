cmake_minimum_required(VERSION 3.16)
project(tr_plugin_icecast CXX)

# Designed to be dropped into trunk-recorder/user_plugins/tr-plugin-icecast/.
# Trunk-recorder's top-level CMakeLists globs user_plugins/* and add_subdirectory()'s
# each one, so most of the build environment (Boost, GNURadio, includes) is inherited.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)

# libmp3lame ships a .pc on most distros; libsamplerate definitely does.
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
# lame's pkg-config module is named 'lame' on Debian/Ubuntu (lame.pc) and
# 'mp3lame' on some other distros; try both, then fall back to a manual search.
pkg_check_modules(MP3LAME IMPORTED_TARGET lame)
if(NOT MP3LAME_FOUND)
    pkg_check_modules(MP3LAME IMPORTED_TARGET mp3lame)
endif()
if(NOT MP3LAME_FOUND)
    find_path(MP3LAME_INCLUDE_DIR lame/lame.h)
    find_library(MP3LAME_LIBRARY mp3lame)
    if(NOT MP3LAME_INCLUDE_DIR OR NOT MP3LAME_LIBRARY)
        message(FATAL_ERROR "libmp3lame not found (apt: libmp3lame-dev)")
    endif()
endif()

add_library(tr_plugin_icecast MODULE
    src/codec/mp3_encoder.cc
    src/codec/resampler.cc
    src/transport/icecast_session.cc
    src/transport/icecast_pool.cc
    src/pipeline/call_state.cc
    src/plugin/config.cc
    src/plugin/icecast_bridge_plugin.cc
)

# Trunk-recorder repo root. Normally this plugin lives two levels down in
# trunk-recorder/user_plugins/<name>/ — but that directory is usually a symlink,
# and "${CMAKE_CURRENT_SOURCE_DIR}/../.." resolves "../.." through the symlink's
# physical target, landing outside the trunk-recorder tree. The include then
# falls through to any stale system-installed trunk-recorder headers in
# /usr/local/include. CMAKE_SOURCE_DIR is the top-level project (= the
# trunk-recorder root when built as a user_plugin) and is symlink-proof.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Standalone build (IDE indexing only — can't link trunk_recorder_library).
    set(TR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
else()
    set(TR_ROOT ${CMAKE_SOURCE_DIR})
endif()

# include paths:
#   src/         -> own modules by full path, e.g. "transport/icecast_session.h"
#   ${TR_ROOT}   -> #include "trunk-recorder/plugin_manager/plugin_api.h"
#   ${TR_ROOT}/lib -> inherited <json.hpp>
target_include_directories(tr_plugin_icecast PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TR_ROOT}
    ${TR_ROOT}/lib
)

# When built standalone (not via trunk-recorder), Boost vars won't be set.
if(NOT Boost_FOUND)
    find_package(Boost REQUIRED COMPONENTS system thread log)
endif()

target_include_directories(tr_plugin_icecast PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(tr_plugin_icecast
    PRIVATE
        trunk_recorder_library
        OpenSSL::SSL
        OpenSSL::Crypto
        PkgConfig::SAMPLERATE
        ${Boost_LIBRARIES}
)

if(MP3LAME_FOUND)
    target_link_libraries(tr_plugin_icecast PRIVATE PkgConfig::MP3LAME)
else()
    target_include_directories(tr_plugin_icecast PRIVATE ${MP3LAME_INCLUDE_DIR})
    target_link_libraries(tr_plugin_icecast PRIVATE ${MP3LAME_LIBRARY})
endif()

install(TARGETS tr_plugin_icecast
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder
)