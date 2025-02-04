/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "src/carnot/udf/doc.h"
#include "src/carnot/udf/udf.h"
#include "src/common/testing/testing.h"

namespace px {
namespace carnot {
namespace udf {

using ::px::testing::proto::EqualsProto;

auto constexpr scalarUDFExpectedDoc = R"(
brief: "This function adds two numbers: c = a + b"
desc: "This function is implicitly invoked by the + operator when applied to a numeric type"
examples {
  value: "df.sum = df.a + df.b"
}
examples {
  value: "df = px.Dataframe(...)\ndf.sum = df.a = df.b\n"
}
scalar_udf_doc {
  args {
    ident: "a"
    desc: "The first argument"
    type: INT64
  }
  args {
    ident: "b"
    desc: "The second argument"
    type: INT64
  }
  retval {
    desc: "The sum of a and b"
    type: INT64
  }
}
)";

class ScalarUDF1 : ScalarUDF {
 public:
  types::Int64Value Exec(FunctionContext*, types::Int64Value, types::Int64Value) { return 0; }
  static ScalarUDFDocBuilder Doc() {
    return ScalarUDFDocBuilder("This function adds two numbers: c = a + b")
        .Details(
            "This function is implicitly invoked by the + operator when applied to a numeric type")
        .Arg("a", "The first argument")
        .Arg("b", "The second argument")
        .Returns("The sum of a and b")
        .Example("df.sum = df.a + df.b")
        .Example(R"(
        | df = px.Dataframe(...)
        | df.sum = df.a = df.b
      )");
  }
};

TEST(doc, scalar_udf_doc_builder) {
  udfspb::Doc doc;
  EXPECT_OK(ScalarUDF1::Doc().ToProto<ScalarUDF1>(&doc));
  EXPECT_THAT(doc, EqualsProto(scalarUDFExpectedDoc));
}

class UDA1 : UDA {
 public:
  Status Init(FunctionContext*) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value) {}
  void Merge(FunctionContext*, const UDA1&) {}
  types::Int64Value Finalize(FunctionContext*) { return 0; }

  static UDADocBuilder Doc() {
    return UDADocBuilder("This function computes the sum of a list of numbers.")
        .Details("The detailed version of this.")
        .Arg("a", "The argument to sum")
        .Returns("The sum of all values of a.")
        .Example("df.sum = df.agg");
  }
};

auto constexpr udaExpectedDoc = R"(
brief: "This function computes the sum of a list of numbers."
desc: "The detailed version of this."
examples {
  value: "df.sum = df.agg"
}
uda_doc {
  update_args {
    ident: "a"
    desc: "The argument to sum"
    type: INT64
  }
 result {
    desc: "The sum of all values of a."
    type: INT64
  }
}
)";

TEST(doc, uda_doc_builder) {
  udfspb::Doc doc;
  EXPECT_OK(UDA1::Doc().ToProto<UDA1>(&doc));
  EXPECT_THAT(doc, EqualsProto(udaExpectedDoc));
}

}  // namespace udf
}  // namespace carnot
}  // namespace px
