# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

#
# Unit tests for the parts of the SIL that work without a bus, most of them parsing what remote devices send
# (the sources in PARSER_SOURCES). They link those sources alone, and GoogleTest from the sysroot.

find_library(GTEST_LIBRARY gtest REQUIRED)
find_library(GTEST_MAIN_LIBRARY gtest_main REQUIRED)

set(PARSER_SOURCES
    ${PROJECT_SOURCE_DIR}/src/meshcomposition.cpp
    ${PROJECT_SOURCE_DIR}/src/leadvfilter.cpp
    ${PROJECT_SOURCE_DIR}/src/obextime.cpp
    ${PROJECT_SOURCE_DIR}/src/dbuspath.cpp
    ${PROJECT_SOURCE_DIR}/src/obexpaths.cpp
    ${PROJECT_SOURCE_DIR}/src/gattlocal.cpp
    ${PROJECT_SOURCE_DIR}/src/meshconfig.cpp
    ${PROJECT_SOURCE_DIR}/src/avrcpdevices.cpp
    ${PROJECT_SOURCE_DIR}/src/gattnotify.cpp
    ${PROJECT_SOURCE_DIR}/src/gattdiscovery.cpp
)

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
add_executable(bluez5-sil-tests ${TEST_SOURCES} ${PARSER_SOURCES})
target_include_directories(bluez5-sil-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bluez5-sil-tests ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY} pthread)
add_test(NAME bluez5-sil-unit COMMAND bluez5-sil-tests)

# Helpers that need GLib (and the SIL's logging): source wrappers, the GVariant array conversions, the bus signals
# that end an OBEX session, and the properties an incoming push is decided on.
file(GLOB GLIB_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/glib/*.cpp)
add_executable(bluez5-sil-glib-tests ${GLIB_TEST_SOURCES}
    ${PROJECT_SOURCE_DIR}/src/asyncutils.cpp ${PROJECT_SOURCE_DIR}/src/utils.cpp
    ${PROJECT_SOURCE_DIR}/src/dbussignals.cpp ${PROJECT_SOURCE_DIR}/src/obexpush.cpp)
target_include_directories(bluez5-sil-glib-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bluez5-sil-glib-tests ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY}
    ${GLIB2_LDFLAGS} ${GIO2_LDFLAGS} ${PMLOG_LDFLAGS} pthread)
add_test(NAME bluez5-sil-glib COMMAND bluez5-sil-glib-tests)

# The AVRCP browsing folder and player, the MPRIS player's registration and SelectRole against a fake bluetoothd on a
# private bus (needs dbus-daemon at run time).
add_executable(bluez5-sil-media-tests ${CMAKE_CURRENT_SOURCE_DIR}/media/test_avrcpmedia.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glib/support.cpp
    ${PROJECT_SOURCE_DIR}/src/bluez5mediafolder.cpp ${PROJECT_SOURCE_DIR}/src/bluez5mediaplayer.cpp
    ${PROJECT_SOURCE_DIR}/src/bluez5mprisplayer.cpp ${PROJECT_SOURCE_DIR}/src/mediarole.cpp
    ${PROJECT_SOURCE_DIR}/src/asyncutils.cpp ${PROJECT_SOURCE_DIR}/src/utils.cpp
    ${GDBUS_IF_DIR}/bluez-interface.c ${GDBUS_IF_DIR}/freedesktop-interface.c)
target_include_directories(bluez5-sil-media-tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${GDBUS_IF_DIR})
target_link_libraries(bluez5-sil-media-tests ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY}
    ${GLIB2_LDFLAGS} ${GIO2_LDFLAGS} ${PMLOG_LDFLAGS} pthread)
# The profile is stubbed, so its type info is not there for UBSan's vptr check (GCC references it).
target_compile_options(bluez5-sil-media-tests PRIVATE -fno-sanitize=vptr)
add_test(NAME bluez5-sil-media COMMAND bluez5-sil-media-tests)

# StartNotify and StopNotify of a remote characteristic against a fake bluetoothd on a private bus (needs
# dbus-daemon at run time).
add_executable(bluez5-sil-gatt-tests ${CMAKE_CURRENT_SOURCE_DIR}/gatt/test_gattremotenotify.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glib/support.cpp
    ${PROJECT_SOURCE_DIR}/src/bluez5gattremoteattribute.cpp ${PROJECT_SOURCE_DIR}/src/gattnotify.cpp
    ${PROJECT_SOURCE_DIR}/src/asyncutils.cpp ${PROJECT_SOURCE_DIR}/src/utils.cpp
    ${GDBUS_IF_DIR}/bluez-interface.c ${GDBUS_IF_DIR}/freedesktop-interface.c)
target_include_directories(bluez5-sil-gatt-tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${GDBUS_IF_DIR})
target_link_libraries(bluez5-sil-gatt-tests ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY}
    ${GLIB2_LDFLAGS} ${GIO2_LDFLAGS} ${PMLOG_LDFLAGS} pthread)
# The profile is stubbed, so its type info is not there for UBSan's vptr check (GCC references it).
target_compile_options(bluez5-sil-gatt-tests PRIVATE -fno-sanitize=vptr)
add_test(NAME bluez5-sil-gatt COMMAND bluez5-sil-gatt-tests)

# libFuzzer targets (clang only, -DBUILD_FUZZING=ON).
option(BUILD_FUZZING "Build the libFuzzer targets (clang)" OFF)
if(BUILD_FUZZING)
    foreach(name meshcomposition leadvfilter obexpaths gattlocal meshconfig)
        add_executable(fuzz-${name} fuzz/fuzz_${name}.cpp ${PROJECT_SOURCE_DIR}/src/${name}.cpp)
        target_include_directories(fuzz-${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_options(fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
        target_link_options(fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
