// Entry point. main only wires things together and holds no logic. The cli
// library owns command parsing and dispatch.

#include "opaquedb/cli/command.h"

int main(int argc, char **argv) { return opaquedb::cli::Run(argc, argv); }
