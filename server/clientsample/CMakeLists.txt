cmake_minimum_required(VERSION 3.16)
project(shadnet-sample VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    cmake_policy(SET CMP0091 NEW)  # Enable runtime library selection
    # Use static runtime for consistency with your protobuf/abseil
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
    # Fix exception handling and UTF-8 warnings
    add_compile_options(/EHsc /utf-8)
endif()

# protobuf: the server's submodule when it is checked out, else the system's (see ../CMakeLists.txt).
set(_sample_protobuf_default OFF)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../externals/protobuf/CMakeLists.txt")
    set(_sample_protobuf_default ON)
endif()
option(SHADNET_SYSTEM_PROTOBUF "Use the system's protobuf instead of ../externals/protobuf"
       ${_sample_protobuf_default})
if(SHADNET_SYSTEM_PROTOBUF)
    find_package(Protobuf CONFIG REQUIRED)
    if(NOT TARGET libprotobuf)
        add_library(libprotobuf INTERFACE)
        target_link_libraries(libprotobuf INTERFACE protobuf::libprotobuf)
    endif()
    set(SAMPLE_PROTOC protobuf::protoc)
    set(SAMPLE_PROTOBUF_INCLUDE "")
else()
    if(NOT TARGET libprotobuf)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../externals" externals)
    endif()
    set(SAMPLE_PROTOC protoc)
    set(SAMPLE_PROTOBUF_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../externals/protobuf/src")
endif()

set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
    OUTPUT
        "${PROTO_OUT}/shadnet.pb.cc"
        "${PROTO_OUT}/shadnet.pb.h"
    COMMAND $<TARGET_FILE:${SAMPLE_PROTOC}>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}"
        "--cpp_out=${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shadnet.proto"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/shadnet.proto"
        ${SAMPLE_PROTOC}
    COMMENT "Generating shadnet protobuf sources"
    VERBATIM
)

add_executable(shadnet-sample
    main.cpp
    client.cpp
    connection.cpp
    "${PROTO_OUT}/shadnet.pb.cc"
)

target_include_directories(shadnet-sample PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROTO_OUT}"
    ${SAMPLE_PROTOBUF_INCLUDE}
)

target_link_libraries(shadnet-sample PRIVATE libprotobuf)

# ── Platform / compiler settings ──────────────────────────────────────────────
if(WIN32)
    target_link_libraries(shadnet-sample PRIVATE ws2_32)
    target_compile_definitions(shadnet-sample PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
endif()

if(MSVC)
    target_compile_options(shadnet-sample PRIVATE /W3 /utf-8)
else()
    target_compile_options(shadnet-sample PRIVATE -Wall -Wextra)
    # A static C++ runtime only with the static submodule protobuf: the system's libprotobuf.so
    # brings the shared one.
    if(NOT SHADNET_SYSTEM_PROTOBUF)
        target_link_options(shadnet-sample PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()
