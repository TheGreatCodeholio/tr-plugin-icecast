cmake_minimum_required(VERSION 3.16)
project(tr_plugin_icecast CXX)

# Designed to be dropped into trunk-recorder/user_plugins/tr-plugin-icecast/.
# Trunk-recorder's top-level CMakeLists globs user_plugins/* and add_subdirectory()'s
# each one, so most of the build environment (Boost, GNURadio, includes) is inherited.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# PkgConfig is found first: CMake's own FindOpenSSL uses pkg_check_modules
# internally, and libsamplerate/LAME detection below depends on it too.
find_package(PkgConfig)
if(NOT PkgConfig_FOUND)
    message(FATAL_ERROR
        "pkg-config not found (needed to locate OpenSSL/libsamplerate/LAME). Install: "
        "Debian/Ubuntu 'pkg-config', Fedora/RHEL 'pkgconf-pkg-config', "
        "Arch 'pkgconf', Alpine 'pkgconf'.")
endif()

find_package(OpenSSL)
if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR
        "OpenSSL not found. Install the development package: "
        "Debian/Ubuntu 'libssl-dev', Fedora/RHEL 'openssl-devel', "
        "Arch 'openssl', Alpine 'openssl-dev'.")
endif()

# libsamplerate ships 'samplerate.pc' on every distro; fall back to a manual
# search (header 'samplerate.h', lib 'libsamplerate') for the rare case it
# doesn't, keeping detection symmetric with LAME below.
pkg_check_modules(SAMPLERATE IMPORTED_TARGET samplerate)
if(NOT SAMPLERATE_FOUND)
    find_path(SAMPLERATE_INCLUDE_DIR samplerate.h)
    find_library(SAMPLERATE_LIBRARY NAMES samplerate)
    if(NOT SAMPLERATE_INCLUDE_DIR OR NOT SAMPLERATE_LIBRARY)
        message(FATAL_ERROR
            "libsamplerate not found. Install the development package: "
            "Debian/Ubuntu 'libsamplerate0-dev', Fedora/RHEL 'libsamplerate-devel', "
            "Arch 'libsamplerate', Alpine 'libsamplerate-dev'.")
    endif()
endif()
# LAME's pkg-config module name varies by distro: 'lame' on Debian/Ubuntu/Arch
# (lame.pc), 'mp3lame' on a few others, and Fedora/RHEL ship no .pc at all. Try
# both module names, then fall back to a manual search for the header + lib,
# which are named 'lame/lame.h' and 'libmp3lame' everywhere on Linux.
pkg_check_modules(MP3LAME IMPORTED_TARGET lame)
if(NOT MP3LAME_FOUND)
    pkg_check_modules(MP3LAME IMPORTED_TARGET mp3lame)
endif()
if(NOT MP3LAME_FOUND)
    find_path(MP3LAME_INCLUDE_DIR lame/lame.h)
    find_library(MP3LAME_LIBRARY NAMES mp3lame)
    if(NOT MP3LAME_INCLUDE_DIR OR NOT MP3LAME_LIBRARY)
        message(FATAL_ERROR
            "libmp3lame not found. Install the LAME development package: "
            "Debian/Ubuntu 'libmp3lame-dev', Fedora/RHEL 'lame-devel', "
            "Arch 'lame', Alpine 'lame-dev'.")
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
# In the normal inherited build, trunk-recorder already found Boost (which is
# why we reuse ${Boost_LIBRARIES} below rather than imported targets — TR does
# not request the 'system' component, so Boost::system would not exist).
if(NOT Boost_FOUND)
    find_package(Boost COMPONENTS system thread log)
    if(NOT Boost_FOUND)
        message(FATAL_ERROR
            "Boost (components: system, thread, log) not found. Install: "
            "Debian/Ubuntu 'libboost-all-dev', Fedora/RHEL 'boost-devel', "
            "Arch 'boost', Alpine 'boost-dev'.")
    endif()
endif()

target_include_directories(tr_plugin_icecast PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(tr_plugin_icecast
    PRIVATE
        trunk_recorder_library
        OpenSSL::SSL
        OpenSSL::Crypto
        ${Boost_LIBRARIES}
)

if(SAMPLERATE_FOUND)
    target_link_libraries(tr_plugin_icecast PRIVATE PkgConfig::SAMPLERATE)
else()
    target_include_directories(tr_plugin_icecast PRIVATE ${SAMPLERATE_INCLUDE_DIR})
    target_link_libraries(tr_plugin_icecast PRIVATE ${SAMPLERATE_LIBRARY})
endif()

if(MP3LAME_FOUND)
    target_link_libraries(tr_plugin_icecast PRIVATE PkgConfig::MP3LAME)
else()
    target_include_directories(tr_plugin_icecast PRIVATE ${MP3LAME_INCLUDE_DIR})
    target_link_libraries(tr_plugin_icecast PRIVATE ${MP3LAME_LIBRARY})
endif()

install(TARGETS tr_plugin_icecast
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder
)