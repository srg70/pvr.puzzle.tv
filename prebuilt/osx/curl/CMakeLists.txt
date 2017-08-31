project(pvr.puzzle.tv)

cmake_minimum_required(VERSION 2.8)

SET(INCLUDE_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/include" CACHE PATH "The directory the headers are installed in")
SET(LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib" CACHE STRING "Directory where lib will install")

install(DIRECTORY include/curl
    DESTINATION "${INCLUDE_INSTALL_DIR}"
)
file(GLOB libs 
    "lib/*.a"
)
foreach(lib ${libs})
    install(FILES "${lib}" DESTINATION ${LIB_INSTALL_DIR})
endforeach()
