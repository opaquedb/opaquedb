#include "opaquedb/core/version.h"

#include <gtest/gtest.h>

namespace {

TEST(CoreVersion, IsNotEmpty) {
  EXPECT_FALSE(opaquedb::core::version().empty());
}

TEST(CoreVersion, MatchesBuildDefinition) {
  EXPECT_EQ(opaquedb::core::version(), OPAQUEDB_VERSION);
}

} // namespace
