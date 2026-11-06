# SPDX-License-Identifier: GPL-2.0-or-later
#
# CMake build of openfat for 32/64-bit hosts and kernels (GDOS). The host
# tests and the OpenWatcom check use the Makefile.
#
# OpenFAT::fat   reading and writing exFAT, formatting FAT12/16/32 and exFAT
#                (needs only string.h)
# OpenFAT::check checking/repairing and defragmenting (also needs stdio.h
#                for its messages)
# As a subproject the parent sets the compiler and flags.

cmake_minimum_required(VERSION 3.20)
project(openfat LANGUAGES C)

add_library(openfat_fat STATIC
  src/exfat/exfat.c
  src/exfat/codepage.c
  src/exfat/exfat_format.c
  src/exfat/upcase.c
  src/fat/fat_format.c)
target_include_directories(openfat_fat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(OpenFAT::fat ALIAS openfat_fat)

add_library(openfat_check STATIC
  src/exfat/exfat_check.c
  src/fat/fat_check.c
  src/fat/fat_defrag.c)
target_link_libraries(openfat_check PUBLIC openfat_fat)
add_library(OpenFAT::check ALIAS openfat_check)

set_target_properties(openfat_fat openfat_check PROPERTIES
  C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
