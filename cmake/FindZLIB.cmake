#.rst:
# FindCurl
# --------
# Finds the Zlib library with NO_CMAKE_FIND_ROOT_PATH
#
# This will will define the following variables::
#
# ZLIB_FOUND - system has Zlib
# ZLIB_INCLUDE_DIRS - the ZLib include directory
# ZLIB_LIBRARIES - the ZLib libraries
#

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_ZLIB libz QUIET)
endif()

message(STATUS "In FindZLIB.cmake CORE_SYSTEM_NAME=${CORE_SYSTEM_NAME}")

find_path(ZLIB_INCLUDE_DIRS NAMES zlib.h
                NO_CMAKE_FIND_ROOT_PATH
                PATHS ${PC_ZLIB_INCLUDEDIR})
find_library(ZLIB_LIBRARIES NAMES z libz
                NO_CMAKE_FIND_ROOT_PATH
                PATHS ${PC_ZLIB_LIBDIR})

set(ZLIB_VERSION ${PC_ZLIB_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Zlib
                                  REQUIRED_VARS ZLIB_LIBRARIES ZLIB_INCLUDE_DIRS
                                  VERSION_VAR ZLIB_VERSION)

mark_as_advanced(ZLIB_INCLUDE_DIRS ZLIB_LIBRARIES)
