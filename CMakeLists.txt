project(pvr.puzzle.tv)

cmake_minimum_required(VERSION 2.6)

enable_language(CXX)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

message(CMAKE_MODULE_PATH=${CMAKE_MODULE_PATH})

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

#${CURL_SSL_LIBRARIES})

message(STATUS "ZLIB_LIBRARIES: ${ZLIB_LIBRARIES}")
message(STATUS "CURL_INCLUDE_DIRS: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL_LIBRARIES: ${CURL_LIBRARIES} ${CURL_SSL_LIBRARIES}")

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
	set(ANDROID_CPP_FEATURES exceptions)
	set(ANDROID_STL c++_static)
	set(DEPLIBS ${DEPLIBS} -static-libstdc++)
endif()

message(STATUS "ANDROID_STL: ${ANDROID_STL}")
message(STATUS "DEPLIBS: ${DEPLIBS}")
message(STATUS "ANDROID_CPP_FEATURES: ${ANDROID_CPP_FEATURES}")

set(IPTV_SOURCES
src/addon.cpp
src/direct_buffer.cpp
src/helpers.cpp
src/pvr_client_base.cpp
src/sovok_pvr_client.cpp
src/sovok_tv.cpp
src/puzzle_pvr_client.cpp
src/puzzle_tv.cpp
src/timeshift_buffer.cpp
src/ActionQueue.cpp
src/HttpEngine.cpp
src/plist_buffer.cpp
src/file_cache_buffer.cpp
src/memory_cache_buffer.cpp
)
#src/ott_pvr_client.cpp
#src/ott_player.cpp


build_addon(pvr.puzzle.tv IPTV DEPLIBS)

include(CPack)
