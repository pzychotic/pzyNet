include(FetchContent)

# Uses an installed Catch2 3.x when there is one, otherwise downloads it.
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS 3)
FetchContent_MakeAvailable(Catch2)
if(catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
include(Catch)

add_executable(pzynet_tests
    core/address_test.cpp
    core/byte_io_test.cpp
    socket/udp_test.cpp
    socket/tcp_test.cpp
    socket/poller_test.cpp
    socket/resolver_test.cpp
    transport/udp_host_test.cpp
    raknet/bit_stream_test.cpp
    raknet/bit_stream_geometry_test.cpp
    raknet/bit_stream_compressed_test.cpp
    raknet/wire_test.cpp
    raknet/engine_test.cpp
    raknet/connection_stats_test.cpp
    rakpeer/peer_test.cpp
    rakpeer/peer_send_test.cpp)
target_link_libraries(pzynet_tests PRIVATE
    pzynet::core pzynet::socket pzynet::transport pzynet::raknet pzynet::rakpeer Catch2::Catch2WithMain)
target_include_directories(pzynet_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
pzynet_configure_target(pzynet_tests)
catch_discover_tests(pzynet_tests PROPERTIES TIMEOUT 120)

# Live wire-compatibility tests against a real RakNet build.
if(PZYNET_RAKNET_INTEROP_DIR)
    if(NOT EXISTS "${PZYNET_RAKNET_INTEROP_DIR}/Source/RakPeerInterface.h")
        message(FATAL_ERROR "PZYNET_RAKNET_INTEROP_DIR='${PZYNET_RAKNET_INTEROP_DIR}' is not a RakNet checkout")
    endif()

    set(RAKNET_ENABLE_DLL OFF CACHE BOOL "" FORCE)
    set(RAKNET_ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(RAKNET_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
    set(RAKNET_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PZYNET_RAKNET_INTEROP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/raknet" EXCLUDE_FROM_ALL)

    add_executable(pzynet_interop_tests
        interop/raknet_interop_test.cpp
        interop/rakpeer_interop_test.cpp
        interop/bit_stream_interop_test.cpp
        interop/bit_stream_geometry_interop_test.cpp
        interop/bit_stream_compressed_interop_test.cpp)
    target_link_libraries(pzynet_interop_tests PRIVATE
        pzynet::transport pzynet::raknet pzynet::rakpeer RakNetStatic Catch2::Catch2WithMain)
    target_include_directories(pzynet_interop_tests SYSTEM PRIVATE "${PZYNET_RAKNET_INTEROP_DIR}/Source")
    target_compile_features(pzynet_interop_tests PRIVATE cxx_std_17)
    catch_discover_tests(pzynet_interop_tests PROPERTIES TIMEOUT 180)

    # The same BitStream comparison against RakNet's BitStream compiled with __BITSTREAM_NATIVE_END, which
    # writes the host's byte order (little-endian here). The define changes code inside RakNet's .cpp
    # files, so those are compiled into this executable directly instead of linking RakNetStatic, which
    # would mix both variants in one program.
    set(raknet_bitstream_sources
        "${PZYNET_RAKNET_INTEROP_DIR}/Source/BitStream.cpp"
        "${PZYNET_RAKNET_INTEROP_DIR}/Source/StringCompressor.cpp"
        "${PZYNET_RAKNET_INTEROP_DIR}/Source/HuffmanEncodingTree.cpp"
        "${PZYNET_RAKNET_INTEROP_DIR}/Source/RakMemoryOverride.cpp"
        "${PZYNET_RAKNET_INTEROP_DIR}/Source/RakNetTypes.cpp"
        "${PZYNET_RAKNET_INTEROP_DIR}/Source/SuperFastHash.cpp"
        "${PZYNET_RAKNET_INTEROP_DIR}/Source/WSAStartupSingleton.cpp")
    # RakNet's own code, not ours: keep its warnings out of the build log.
    set_source_files_properties(${raknet_bitstream_sources} PROPERTIES COMPILE_OPTIONS $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
    add_executable(pzynet_interop_native_end_tests
        interop/bit_stream_interop_test.cpp
        interop/bit_stream_geometry_interop_test.cpp
        interop/bit_stream_compressed_interop_test.cpp
        ${raknet_bitstream_sources})
    target_link_libraries(pzynet_interop_native_end_tests PRIVATE pzynet::raknet Catch2::Catch2WithMain)
    target_include_directories(pzynet_interop_native_end_tests SYSTEM PRIVATE "${PZYNET_RAKNET_INTEROP_DIR}/Source")
    target_compile_definitions(pzynet_interop_native_end_tests PRIVATE
        __BITSTREAM_NATIVE_END PZYNET_ORACLE_NATIVE_END _RAKNET_LIB _CRT_SECURE_NO_WARNINGS)
    target_compile_features(pzynet_interop_native_end_tests PRIVATE cxx_std_17)
    if(WIN32)
        target_link_libraries(pzynet_interop_native_end_tests PRIVATE ws2_32)
    endif()
    catch_discover_tests(pzynet_interop_native_end_tests TEST_PREFIX "native-end: " PROPERTIES TIMEOUT 180)
endif()
