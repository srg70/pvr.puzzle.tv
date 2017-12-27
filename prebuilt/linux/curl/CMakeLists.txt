project(pvr.puzzle.tv)

cmake_minimum_required(VERSION 2.8)

SET(INCLUDE_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/include" CACHE PATH "The directory the headers are installed in")
SET(LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib" CACHE STRING "Directory where lib will install")

# Find out linux architecture subfolder.
# We'll parce DEFAULT_DOCKCROSS_IMAGE env variable
# expected value is like dockcross/linux-armv7

set(linux_platform_regex "dockcross/(.*)")
string(REGEX MATCH
    "${source_and_after_regex}"
    dockross_and_after "${DEFAULT_DOCKCROSS_IMAGE}")

if(dockross_and_after)
    string(REGEX REPLACE
    "${dockross_and_after_regex}"
    "\\1"
    linux_platform "${dockross_and_after}")
    if(NOT EXISTS ${linux_platform})
        message(FATAL_ERROR "Can not install CURL library for ${linux_platform}!")
    endif()
else(dockross_and_after)
    message(FATAL_ERROR "Can not determine platform from ${DEFAULT_DOCKCROSS_IMAGE}!")
endif(dockross_and_after)

install(DIRECTORY ${linux_platform}/include/curl
    DESTINATION "${INCLUDE_INSTALL_DIR}"
)
file(GLOB libs 
    "${linux_platform}/lib/*.a"
)
foreach(lib ${libs})
    install(FILES "${lib}" DESTINATION ${LIB_INSTALL_DIR})
endforeach()
