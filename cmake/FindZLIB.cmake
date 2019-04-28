#.rst:
# FindZlib (override default CMAKE script)
# --------
# Finds the ZLib library
#
# This will will define the following variables::
#
# ZLIB_FOUND - system has ZLib
# ZLIB_INCLUDE_DIRS - the Zlib include directory
# ZLIB_LIBRARIES - the ZLib libraries
# ZLIB_DEFINITIONS - the ZLib definitions
#
# and the following imported targets::
#
#   ZLib::ZLib   - The ZLib library

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_ZLIB libz QUIET)
endif()

# message(STATUS "In FindCURL.cmake CORE_SYSTEM_NAME=${CORE_SYSTEM_NAME}")

#if(CORE_SYSTEM_NAME STREQUAL osx)
#    find_path(CURL_INCLUDE_DIRS NAMES curl/curl.h
#                               PATHS ${PC_CURL_INCLUDEDIR})
#    find_library(CURL_LIBRARIES NAMES curl libcurl
#                                PATHS ${PC_CURL_LIBDIR})
if(CORE_SYSTEM_NAME STREQUAL windows)
    find_path(ZLIB_INCLUDE_DIRS NAMES zlib.h
                               PATHS ${PC_ZLIB_INCLUDEDIR})
    find_library(ZLIB_LIBRARIES NAMES z zlib
                                PATHS ${PC_ZLIB_LIBDIR})
else()
    find_path(ZLIB_INCLUDE_DIRS NAMES zlib.h
                    NO_CMAKE_FIND_ROOT_PATH
                    PATHS ${PC_ZLIB_INCLUDEDIR})
    find_library(ZLIB_LIBRARIES NAMES z libz
                    NO_CMAKE_FIND_ROOT_PATH
                    PATHS ${PC_ZLIB_LIBDIR})
endif()


set(ZLIB_VERSION ${PC_ZLIB_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZLib
                                  REQUIRED_VARS ZLIB_LIBRARIES ZLIB_INCLUDE_DIRS
                                  VERSION_VAR ZLIB_VERSION)

mark_as_advanced(ZLIB_INCLUDE_DIRS ZLIB_LIBRARIES)
