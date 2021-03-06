#
# Copyright 2015 by Ben Zhang
#
# Author(s):
#   Ben Zhang, May 2016
#
# Try to find GRT
# Once done this will define
#  GRT_FOUND          - System has Grt
#  GRT_INCLUDE_DIR    - The Grt include directories
#  GRT_LIBRARY        - The libraries needed to use Grt
#

find_package(PkgConfig)
pkg_check_modules(PC_GRT QUIET libgrt)

find_path(GRT_INCLUDE_DIR
  NAMES GRT/GRT.h
  HINTS ${PC_LIBDIR} ${PC_LIBRARY_DIRS} ${_GRT_LIBRARY_DIR}
  ${PC_GRT_INCLUDEDIR} ${PC_GRT_INCLUDE_DIRS}
  PATH_SUFFIXES grt
  )

find_library(
  GRT_LIBRARY NAMES grt
  HINTS ${PC_GRT_LIBDIR} ${PC_GRT_LIBRARY_DIRS}
  )

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  grt
  REQUIRED_VARS GRT_LIBRARY GRT_INCLUDE_DIR
  )

if(GRT_LIBRARY AND GRT_INCLUDE_DIR)
  SET(GRT_FOUND TRUE)
else(GRT_LIBRARY AND GRT_INCLUDE_DIR)
  SET(GRT_FOUND FALSE)
endif(GRT_LIBRARY AND GRT_INCLUDE_DIR)

mark_as_advanced(GRT_INCLUDE_DIR GRT_LIBRARY)
