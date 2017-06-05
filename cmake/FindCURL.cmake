#.rst:
# FindCurl
# --------
# Finds the Curl library
#
# This will will define the following variables::
#
# CURL_FOUND - system has Curl
# CURL_INCLUDE_DIRS - the Curl include directory
# CURL_LIBRARIES - the Curl libraries
# CURL_DEFINITIONS - the Curl definitions
#
# and the following imported targets::
#
#   Curl::Curl   - The Curl library

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_CURL libcurl QUIET)
endif()

if(CORE_SYSTEM_NAME STREQUAL osx)
    find_path(CURL_INCLUDE_DIRS NAMES curl/curl.h
                               PATHS ${PC_CURL_INCLUDEDIR})
    find_library(CURL_LIBRARIES NAMES curl libcurl 
                              PATHS ${PC_CURL_LIBDIR})
else()
    find_path(CURL_INCLUDE_DIRS NAMES curl/curl.h NO_CMAKE_FIND_ROOT_PATH
                    PATHS ${PC_CURL_INCLUDEDIR})
    find_library(CURL_LIBRARIES NAMES curl libcurl libcurl_a NO_CMAKE_FIND_ROOT_PATH
                    PATHS ${PC_CURL_LIBDIR})

endif()


set(CURL_VERSION ${PC_CURL_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Curl
                                  REQUIRED_VARS CURL_LIBRARIES CURL_INCLUDE_DIRS
                                  VERSION_VAR CURL_VERSION)

mark_as_advanced(CURL_INCLUDE_DIRS CURL_LIBRARIES)
