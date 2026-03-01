cmake_minimum_required(VERSION 3.14)
project(capnproto-lean CXX)

set(CMAKE_CXX_STANDARD 23)

find_package(CapnProto REQUIRED)

add_executable(capnpc-lean4 plugin/capnpc-lean4.c++)
target_link_libraries(capnpc-lean4 PRIVATE CapnProto::capnp CapnProto::kj)

# Optional installation step
install(TARGETS capnpc-lean4 DESTINATION bin)
