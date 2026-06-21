# Generated proto and gRPC code. The .proto files are the single source of truth
# for the wire format; the generated C++ lives only in the build tree and is
# never committed. Consumers get the generated headers as system includes so our
# strict warning flags do not apply to generated code.
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_library(opaquedb_proto
  opaquedb.proto
  admin.proto
  internal.proto)

target_link_libraries(opaquedb_proto
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
    gRPC::grpc++_reflection)

target_include_directories(opaquedb_proto SYSTEM PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

protobuf_generate(
  TARGET opaquedb_proto
  LANGUAGE cpp)

protobuf_generate(
  TARGET opaquedb_proto
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
