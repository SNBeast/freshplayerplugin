cmake_minimum_required(VERSION 3.15)
project(freshplayerplugin)

# version
set(VER_MAJOR 0)
set(VER_MINOR 5)
set(VER_MICRO 0)

# setup compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -fPIC -pthread")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror=format-security")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=implicit-function-declaration")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fPIC -pthread")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror=format-security")

# add missing NDEBUG in RelWithDebugInfo
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -DNDEBUG -g3")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -DNDEBUG -g3")

# generate more debug info for Debug build type
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g3")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g3")

# directory for auto-generated headers
include_directories("${CMAKE_BINARY_DIR}")

# determine software version
add_custom_target(git-revision ALL
                  COMMAND sh "${CMAKE_SOURCE_DIR}/cmake/update-version-info.sh"
                  "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}" ${VER_MAJOR}
                  ${VER_MINOR} ${VER_MICRO}
                  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

# find Ragel
find_program(RAGEL ragel)
if (${RAGEL} STREQUAL "RAGEL-NOTFOUND")
    message(FATAL_ERROR "Ragel State Machine Compiler not found")
endif()

# dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB glib-2.0 REQUIRED)
pkg_check_modules(REQ
    gio-2.0
    x11
    xrandr
    xrender
    xcursor
    gl
    libdrm
    libavcodec
    libavutil
    libswscale
    libv4l2
    cairo
    pango
    pangocairo
    pangoft2
    freetype2
    REQUIRED)
pkg_check_modules(DYLIB
    alsa
    libpulse
    REQUIRED)

# merge glib-2.0
list(APPEND REQ_LIBRARY_DIRS ${GLIB_LIBRARY_DIRS})
list(APPEND REQ_INCLUDE_DIRS ${GLIB_INCLUDE_DIRS})
list(APPEND REQ_LIBRARIES    ${GLIB_LIBRARIES})

# merge dynamic dependencies
list(APPEND REQ_INCLUDE_DIRS ${DYLIB_INCLUDE_DIRS})

# libdl.so, librt.so, and libm.so
list(APPEND REQ_LIBRARIES ${CMAKE_DL_LIBS} rt m)

# OpenGL ES 2
set(WITH_GLES2 FALSE CACHE BOOLEAN "use system libGLESv2")
if (WITH_GLES2)
    pkg_check_modules(GLES2 glesv2 REQUIRED)
    add_definitions(-DHAVE_GLES2=1)
    list(APPEND REQ_LIBRARY_DIRS ${GLES2_LIBRARY_DIRS})
    list(APPEND REQ_INCLUDE_DIRS ${GLES2_INCLUDE_DIRS})
    list(APPEND REQ_LIBRARIES    ${GLES2_LIBRARIES})
else()
    # otherwise ANGLE will be used for shader translation
endif()

include_directories(${REQ_INCLUDE_DIRS})
link_directories(${REQ_LIBRARY_DIRS})
add_definitions(${REQ_CFLAGS_OTHER})
add_definitions(-DXP_UNIX)

include_directories(3rdparty)

include(FetchContent)

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

set(EVENT__LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
set(EVENT__DISABLE_OPENSSL ON CACHE BOOL "" FORCE)
set(EVENT__DISABLE_BENCHMARK ON CACHE BOOL "" FORCE)
set(EVENT__DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(EVENT__DISABLE_SAMPLES ON CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

FetchContent_Declare(
    libevent
    GIT_REPOSITORY https://github.com/libevent/libevent
    GIT_TAG        release-2.1.13-stable
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libevent)

# merge openssl
list(APPEND REQ_LIBRARIES    OpenSSL::Crypto)

# merge libevent
list(APPEND REQ_LIBRARIES    event_pthreads_static event_extra_static event_core_static Threads::Threads)

if (NOT WITH_GLES2)
    include_directories(3rdparty/angle/include)
    list(APPEND REQ_LIBRARIES shader_translator)
endif()

set(SYMBOLMAP "-Wl,-version-script=\"${CMAKE_SOURCE_DIR}/src/symbolmap\"")

add_subdirectory(3rdparty)
add_subdirectory(src)
add_subdirectory(tests)
