project(pvr.puzzle.tv)

cmake_minimum_required(VERSION 2.6)

enable_language(CXX)

find_package(Kodi REQUIRED)
find_package(kodiplatform REQUIRED)
find_package(p8-platform REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

include_directories(${kodiplatform_INCLUDE_DIRS}
                    ${p8-platform_INCLUDE_DIRS}
                    ${KODI_INCLUDE_DIR}
                    ${ZLIB_INCLUDE_DIRS}
                    ${CURL_INCLUDE_DIRS}
                    ${PROJECT_SOURCE_DIR}/lib)

set(DEPLIBS ${p8-platform_LIBRARIES}
            ${ZLIB_LIBRARIES}
            ${CURL_LIBRARIES})

message(STATUS "ZLIB_LIBRARIES: ${ZLIB_LIBRARIES}")
message(STATUS "CURL_INCLUDE_DIRS: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL_INCLUDE_DIRS: ${CURL_INCLUDE_DIRS}")

set(IPTV_SOURCES
src/addon.cpp
src/direct_buffer.cpp
src/helpers.cpp
src/sovok_data_source.cpp
src/sovok_pvr_client.cpp
src/sovok_tv.cpp
src/timeshift_buffer.cpp
src/ActionQueue.cpp
)

build_addon(pvr.puzzle.tv IPTV DEPLIBS)

include(CPack)
