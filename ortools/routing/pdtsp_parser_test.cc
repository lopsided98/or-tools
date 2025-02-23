// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/routing/pdtsp_parser.h"

#include <functional>
#include <string>

#include "absl/flags/flag.h"
#include "gtest/gtest.h"
#include "ortools/base/helpers.h"
#include "ortools/base/path.h"
#include "ortools/base/types.h"

#if defined(_MSC_VER)
#define ROOT_DIR "../../../../../../../"
#else
#define ROOT_DIR
#endif  // _MSC_VER

ABSL_FLAG(std::string, test_srcdir, "", "REQUIRED: src dir");

namespace operations_research {
namespace {
TEST(PdTspParserTest, LoadDataSet) {
  for (const std::string& data : {
           ROOT_DIR "ortools/routing/testdata/"
                    "pdtsp_prob10b.txt",
       }) {
    PdTspParser parser;
    EXPECT_TRUE(parser.LoadFile(
        file::JoinPath(absl::GetFlag(FLAGS_test_srcdir), data)));
    EXPECT_EQ(0, parser.depot());
    EXPECT_EQ(21, parser.Size());
    EXPECT_FALSE(parser.IsPickup(0));   // depot
    EXPECT_FALSE(parser.IsPickup(11));  // delivery
    EXPECT_TRUE(parser.IsPickup(2));    // pickup
    EXPECT_EQ(12, parser.DeliveryFromPickup(2));
    std::function<int64_t(int, int)> distances = parser.Distances();
    for (int i = 0; i < parser.Size(); ++i) {
      EXPECT_EQ(0, distances(i, i));
    }
    EXPECT_EQ(557, distances(1, 20));
  }
}
}  // namespace
}  // namespace operations_research
