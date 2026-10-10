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
# LE scan filter matchers and OBEX listing times. They link those sources alone, and GoogleTest from the sysroot.

find_library(GTEST_LIBRARY gtest REQUIRED)
find_library(GTEST_MAIN_LIBRARY gtest_main REQUIRED)

set(PARSER_SOURCES ${PROJECT_SOURCE_DIR}/src/meshcomposition.cpp ${PROJECT_SOURCE_DIR}/src/leadvfilter.cpp
    ${PROJECT_SOURCE_DIR}/src/obextime.cpp)

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
add_executable(bluez5-sil-tests ${TEST_SOURCES} ${PARSER_SOURCES})
target_include_directories(bluez5-sil-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bluez5-sil-tests ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY} pthread)
add_test(NAME bluez5-sil-unit COMMAND bluez5-sil-tests)

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
