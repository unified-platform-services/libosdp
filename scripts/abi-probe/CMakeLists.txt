#
# Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.14)
project(osdp_abi_probe C)

find_package(libosdp CONFIG REQUIRED)
add_executable(abi-probe abi-probe.c)
target_link_libraries(abi-probe PRIVATE libosdp::osdp)
