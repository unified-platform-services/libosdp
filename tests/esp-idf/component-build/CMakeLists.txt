#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#
# Build-only ESP-IDF project that proves esp-idf/component.cmake still compiles
# and links. It is not packaged (idf_component.yml excludes tests/) and it is
# never run -- the release workflow gates the registry upload on this building.
#
# libosdp is picked up from components/libosdp, a symlink to the repo root that
# the release workflow creates (see README.md to make it by hand). The link is
# required rather than cosmetic: ESP-IDF derives a component's name from its
# directory basename, and the checkout directory is named after the repository
# only by convention.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(libosdp-build-test)

# component.cmake sets only -Wall -Wextra, so a warning in library code would
# not fail the build. Promote them here rather than there: -Werror belongs to
# this gate, not to applications that consume the published component.
idf_component_get_property(libosdp_lib libosdp COMPONENT_LIB)
target_compile_options(${libosdp_lib} PRIVATE -Werror)
