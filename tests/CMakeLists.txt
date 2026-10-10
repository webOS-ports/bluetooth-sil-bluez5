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
# Unit tests for what the SIL parses from remote devices without a bus: Composition Data from mesh nodes, the
# LE scan filter matchers, OBEX listing times and MPRIS times. They link those sources alone, and GoogleTest
# from the sysroot.

find_library(GTEST_LIBRARY gtest REQUIRED)
find_library(GTEST_MAIN_LIBRARY gtest_main REQUIRED)

set(PARSER_SOURCES ${PROJECT_SOURCE_DIR}/src/meshcomposition.cpp ${PROJECT_SOURCE_DIR}/src/leadvfilter.cpp
    ${PROJECT_SOURCE_DIR}/src/obextime.cpp ${PROJECT_SOURCE_DIR}/src/dbuspath.cpp)

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
add_executable(bluez5-sil-tests ${TEST_SOURCES} ${PARSER_SOURCES})
target_include_directories(bluez5-sil-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bluez5-sil-tests ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY} pthread)
add_test(NAME bluez5-sil-unit COMMAND bluez5-sil-tests)

# Helpers that need GLib (and the SIL's logging): source wrappers and the GVariant array conversions.
file(GLOB GLIB_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/glib/*.cpp)
add_executable(bluez5-sil-glib-tests ${GLIB_TEST_SOURCES}
    ${PROJECT_SOURCE_DIR}/src/asyncutils.cpp ${PROJECT_SOURCE_DIR}/src/utils.cpp)
target_include_directories(bluez5-sil-glib-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bluez5-sil-glib-tests ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY}
    ${GLIB2_LDFLAGS} ${GIO2_LDFLAGS} ${PMLOG_LDFLAGS} pthread)
add_test(NAME bluez5-sil-glib COMMAND bluez5-sil-glib-tests)

# libFuzzer targets (clang only, -DBUILD_FUZZING=ON).
option(BUILD_FUZZING "Build the libFuzzer targets (clang)" OFF)
if(BUILD_FUZZING)
    foreach(name meshcomposition leadvfilter)
        add_executable(fuzz-${name} fuzz/fuzz_${name}.cpp ${PROJECT_SOURCE_DIR}/src/${name}.cpp)
        target_include_directories(fuzz-${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_options(fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
        target_link_options(fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
