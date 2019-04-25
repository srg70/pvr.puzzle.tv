#.rst:
# FindTAR
# --------
# Finds the TAR library
#
# This will will define the following variables::
#
# TAR_FOUND - system has TAR
# TAR_INCLUDE_DIRS - the TAR include directory
# TAR_LIBRARIES - the TAR libraries
#
# and the following imported targets::
#
#   Tar::Tar   - The Tar library

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_TAR libtar QUIET)
endif()

# message(STATUS "In FindTAR.cmake CORE_SYSTEM_NAME=${CORE_SYSTEM_NAME}")

#if(CORE_SYSTEM_NAME STREQUAL osx)
    find_path(TAR_INCLUDE_DIRS NAMES libtar.h
                               PATHS ${PC_TAR_INCLUDEDIR})
    find_library(TAR_LIBRARIES NAMES tar libtar
                                PATHS ${PC_TAR_LIBDIR})
#endif()


set(TAR_VERSION ${PC_TAR_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Tar
                                  REQUIRED_VARS TAR_LIBRARIES TAR_INCLUDE_DIRS
                                  VERSION_VAR TAR_VERSION)

mark_as_advanced(TAR_INCLUDE_DIRS TAR_LIBRARIES)
