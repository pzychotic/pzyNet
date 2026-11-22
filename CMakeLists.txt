cmake_minimum_required(VERSION 3.24) # 3.24: FetchContent FIND_PACKAGE_ARGS, PROJECT_IS_TOP_LEVEL

project(pzynet VERSION 0.1.0 LANGUAGES CXX)

option(PZYNET_BUILD_TESTS "Build the Catch2 test suite." ${PROJECT_IS_TOP_LEVEL})
option(PZYNET_BUILD_EXAMPLES "Build the example programs." ${PROJECT_IS_TOP_LEVEL})
option(PZYNET_WARNINGS_AS_ERRORS "Treat compiler warnings as errors." OFF)
option(PZYNET_INSTALL "Generate install rules." ${PROJECT_IS_TOP_LEVEL})
set(PZYNET_RAKNET_INTEROP_DIR "" CACHE PATH
    "Path to a RakNet checkout. When set, the live wire-compatibility tests are built against it.")

# Common settings for every target of this project.
function(pzynet_configure_target target)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        target_compile_features(${target} INTERFACE cxx_std_17)
        return()
    endif()

    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        if(PZYNET_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(PZYNET_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(pzynet_add_library name)
    add_library(pzynet_${name} ${ARGN})
    add_library(pzynet::${name} ALIAS pzynet_${name})
    set_target_properties(pzynet_${name} PROPERTIES EXPORT_NAME ${name} OUTPUT_NAME pzynet_${name})
    target_include_directories(pzynet_${name} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    pzynet_configure_target(pzynet_${name})
endfunction()

# --- core: byte views, error helpers, byte/bit IO, addresses -------------------------------------------
pzynet_add_library(core STATIC
    src/core/address.cpp)

# --- socket: UDP/TCP sockets, poller, resolver. Usable on its own. --------------------------------------
pzynet_add_library(socket STATIC
    src/socket/socket.cpp
    src/socket/resolver.cpp
    $<IF:$<PLATFORM_ID:Windows>,src/socket/poller_win32.cpp,src/socket/poller_epoll.cpp>)
target_link_libraries(pzynet_socket PUBLIC pzynet::core)
if(WIN32)
    target_link_libraries(pzynet_socket PRIVATE ws2_32)
endif()

# --- transport: protocol-neutral engine interface + UDP driver -----------------------------------------
find_package(Threads REQUIRED)
pzynet_add_library(transport STATIC
    src/transport/udp_host.cpp)
target_link_libraries(pzynet_transport PUBLIC pzynet::core pzynet::socket Threads::Threads)

# --- raknet: RakNet 4.x wire protocol as a sans-I/O engine ---------------------------------------------
# Needs only the *headers* of the transport layer (the Engine interface); it never touches a socket.
pzynet_add_library(raknet STATIC
    src/raknet/bit_stream.cpp
    src/raknet/wire.cpp
    src/raknet/session.cpp
    src/raknet/raknet_engine.cpp)
target_link_libraries(pzynet_raknet PUBLIC pzynet::core)

# --- rakpeer: RakNet-style Peer (socket + engine + background thread) on top of raknet --------------------
pzynet_add_library(rakpeer STATIC
    src/rakpeer/peer.cpp)
target_link_libraries(pzynet_rakpeer PUBLIC pzynet::raknet pzynet::transport pzynet::socket)

if(PZYNET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PZYNET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(PZYNET_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS pzynet_core pzynet_socket pzynet_transport pzynet_raknet pzynet_rakpeer
        EXPORT pzynetTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT pzynetTargets NAMESPACE pzynet:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pzynet)

    configure_package_config_file(cmake/pzynetConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/pzynetConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pzynet)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/pzynetConfigVersion.cmake
        COMPATIBILITY SameMinorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/pzynetConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/pzynetConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pzynet)
endif()
