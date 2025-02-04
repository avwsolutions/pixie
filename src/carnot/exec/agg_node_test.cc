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

#include "src/carnot/exec/agg_node.h"

#include <algorithm>

#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>
#include <sole.hpp>

#include "src/carnot/exec/test_utils.h"
#include "src/carnot/planpb/plan.pb.h"
#include "src/carnot/udf/registry.h"
#include "src/common/testing/testing.h"
#include "src/shared/types/typespb/wrapper/types_pb_wrapper.h"

namespace px {
namespace carnot {
namespace exec {

using table_store::schema::RowDescriptor;
using ::testing::_;
using types::Int64Value;
using udf::FunctionContext;

// Test UDA, takes the min of two arguments and then sums them.
// TODO(zasgar): move these all to a common file.
class MinSumUDA : public udf::UDA {
 public:
  void Update(udf::FunctionContext*, types::Int64Value arg1, types::Int64Value arg2) {
    sum_ = sum_.val + std::min(arg1.val, arg2.val);
  }
  void Merge(udf::FunctionContext*, const MinSumUDA& other) { sum_ = sum_.val + other.sum_.val; }
  types::Int64Value Finalize(udf::FunctionContext*) { return sum_; }

 protected:
  types::Int64Value sum_ = 0;
};

constexpr char kBlockingNoGroupAgg[] = R"(
op_type: AGGREGATE_OPERATOR
agg_op {
  windowed: false
  values {
    name: "minsum"
    args {
      column {
        node:0
        index: 0
      }
    }
    args {
      column {
        node:0
        index: 1
      }
    }
  }
  value_names: "value1"
})";

constexpr char kBlockingSingleGroupAgg[] = R"(
op_type: AGGREGATE_OPERATOR
agg_op {
  windowed: false
  values {
    name: "minsum"
    args {
      column {
        node:0
        index: 0
      }
    }
    args {
      column {
        node:0
        index: 1
      }
    }
  }
  groups {
     node: 0
     index: 0
  }
  group_names: "g1"
  value_names: "value1"
})";

constexpr char kBlockingMultipleGroupAgg[] = R"(
op_type: AGGREGATE_OPERATOR
agg_op {
  windowed: false
  values {
    name: "minsum"
    args {
      column {
        node:0
        index: 2
      }
    }
    args {
      column {
        node:0
        index: 1
      }
    }
  }
  groups {
     node: 0
     index: 0
  }
  groups {
     node: 0
     index: 1
  }
  group_names: "g1"
  group_names: "g2"
  value_names: "value1"
})";

constexpr char kWindowedNoGroupAgg[] = R"(
op_type: AGGREGATE_OPERATOR
agg_op {
  windowed: true
  values {
    name: "minsum"
    args {
      column {
        node:0
        index: 0
      }
    }
    args {
      column {
        node:0
        index: 1
      }
    }
  }
  value_names: "value1"
})";

constexpr char kWindowedSingleGroupAgg[] = R"(
op_type: AGGREGATE_OPERATOR
agg_op {
  windowed: true
  values {
    name: "minsum"
    args {
      column {
        node:0
        index: 0
      }
    }
    args {
      column {
        node:0
        index: 1
      }
    }
  }
  groups {
     node: 0
     index: 0
  }
  group_names: "g1"
  value_names: "value1"
})";

constexpr char kSingleGroupNoValues[] = R"(
op_type: AGGREGATE_OPERATOR
agg_op {
  groups {
     node: 0
     index: 0
  }
  group_names: "g1"
})";

std::unique_ptr<ExecState> MakeTestExecState(udf::Registry* registry) {
  auto table_store = std::make_shared<table_store::TableStore>();
  return std::make_unique<ExecState>(registry, table_store, MockResultSinkStubGenerator,
                                     sole::uuid4(), nullptr);
}

std::unique_ptr<plan::Operator> PlanNodeFromPbtxt(const std::string& pbtxt) {
  planpb::Operator op_pb;
  EXPECT_TRUE(google::protobuf::TextFormat::MergeFromString(pbtxt, &op_pb));
  return plan::AggregateOperator::FromProto(op_pb, 1);
}

class AggNodeTest : public ::testing::Test {
 public:
  AggNodeTest() {
    func_registry_ = std::make_unique<udf::Registry>("test");
    EXPECT_TRUE(func_registry_->Register<MinSumUDA>("minsum").ok());

    exec_state_ = MakeTestExecState(func_registry_.get());
    EXPECT_OK(exec_state_->AddUDA(0, "minsum",
                                  std::vector<types::DataType>({types::INT64, types::INT64})));
  }

 protected:
  std::unique_ptr<ExecState> exec_state_;
  std::unique_ptr<udf::Registry> func_registry_;
};

TEST_F(AggNodeTest, no_groups_blocking) {
  auto plan_node = PlanNodeFromPbtxt(kBlockingNoGroupAgg);
  RowDescriptor input_rd({types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd({types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::Int64Value>({1, 2, 3, 4})
                       .AddColumn<types::Int64Value>({2, 5, 6, 8})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, true)
                       .AddColumn<types::Int64Value>({5, 6, 3, 4})
                       .AddColumn<types::Int64Value>({1, 5, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 1, true, true)
                          .AddColumn<types::Int64Value>({Int64Value(23)})
                          .get(),
                      false)
      .Close();
}

TEST_F(AggNodeTest, zero_row_row_batch) {
  auto plan_node = PlanNodeFromPbtxt(kBlockingNoGroupAgg);
  RowDescriptor input_rd({types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd({types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::Int64Value>({1, 2, 3, 4})
                       .AddColumn<types::Int64Value>({2, 5, 6, 8})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 0, true, true)
                       .AddColumn<types::Int64Value>({})
                       .AddColumn<types::Int64Value>({})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 1, true, true)
                          .AddColumn<types::Int64Value>({Int64Value(10)})
                          .get(),
                      false)
      .Close();
}

TEST_F(AggNodeTest, single_group_blocking) {
  auto plan_node = PlanNodeFromPbtxt(kBlockingSingleGroupAgg);
  RowDescriptor input_rd({types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd({types::DataType::INT64, types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::Int64Value>({1, 1, 2, 2})
                       .AddColumn<types::Int64Value>({2, 3, 3, 1})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, true)
                       .AddColumn<types::Int64Value>({5, 6, 3, 4})
                       .AddColumn<types::Int64Value>({1, 5, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 6, true, true)
                          .AddColumn<types::Int64Value>({1, 2, 3, 4, 5, 6})
                          .AddColumn<types::Int64Value>({2, 3, 3, 4, 1, 5})
                          .get(),
                      false)
      .Close();
}

TEST_F(AggNodeTest, multiple_groups_blocking) {
  auto plan_node = PlanNodeFromPbtxt(kBlockingMultipleGroupAgg);
  RowDescriptor input_rd({types::DataType::INT64, types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd({types::DataType::INT64, types::DataType::INT64, types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::Int64Value>({1, 5, 1, 2})
                       .AddColumn<types::Int64Value>({2, 1, 3, 1})
                       .AddColumn<types::Int64Value>({2, 5, 3, 1})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, true)
                       .AddColumn<types::Int64Value>({5, 1, 3, 3})
                       .AddColumn<types::Int64Value>({1, 2, 3, 3})
                       .AddColumn<types::Int64Value>({1, 3, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 5, true, true)
                          .AddColumn<types::Int64Value>({1, 1, 2, 5, 3})
                          .AddColumn<types::Int64Value>({2, 3, 1, 1, 3})
                          .AddColumn<types::Int64Value>({4, 3, 1, 2, 6})
                          .get(),
                      false)
      .Close();
}

TEST_F(AggNodeTest, multiple_groups_with_string_blocking) {
  auto plan_node = PlanNodeFromPbtxt(kBlockingMultipleGroupAgg);
  RowDescriptor input_rd({types::DataType::STRING, types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd(
      {types::DataType::STRING, types::DataType::INT64, types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::StringValue>({"abc", "def", "abc", "fgh"})
                       .AddColumn<types::Int64Value>({2, 1, 3, 1})
                       .AddColumn<types::Int64Value>({2, 5, 3, 1})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, true)
                       .AddColumn<types::StringValue>({"ijk", "abc", "abc", "def"})
                       .AddColumn<types::Int64Value>({1, 2, 3, 3})
                       .AddColumn<types::Int64Value>({1, 3, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 6, true, true)
                          .AddColumn<types::StringValue>({"abc", "def", "abc", "fgh", "ijk", "def"})
                          .AddColumn<types::Int64Value>({2, 1, 3, 1, 1, 3})
                          .AddColumn<types::Int64Value>({4, 1, 6, 1, 1, 3})
                          .get(),
                      false)
      .Close();
}

TEST_F(AggNodeTest, no_groups_windowed) {
  auto plan_node = PlanNodeFromPbtxt(kWindowedNoGroupAgg);
  RowDescriptor input_rd({types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd({types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::Int64Value>({1, 2, 3, 4})
                       .AddColumn<types::Int64Value>({2, 5, 6, 8})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, false)
                       .AddColumn<types::Int64Value>({5, 6, 3, 4})
                       .AddColumn<types::Int64Value>({1, 5, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 1, true, false)
                          .AddColumn<types::Int64Value>({Int64Value(23)})
                          .get(),
                      false)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, false, false)
                       .AddColumn<types::Int64Value>({1, 2, 3, 4})
                       .AddColumn<types::Int64Value>({2, 5, 6, 8})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, true)
                       .AddColumn<types::Int64Value>({5, 6, 3, 4})
                       .AddColumn<types::Int64Value>({1, 5, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 1, true, true)
                          .AddColumn<types::Int64Value>({Int64Value(23)})
                          .get(),
                      false)
      .Close();
}

TEST_F(AggNodeTest, single_group_windowed) {
  auto plan_node = PlanNodeFromPbtxt(kWindowedSingleGroupAgg);
  RowDescriptor input_rd({types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd({types::DataType::INT64, types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::Int64Value>({1, 1, 2, 2})
                       .AddColumn<types::Int64Value>({2, 3, 3, 1})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, false)
                       .AddColumn<types::Int64Value>({5, 6, 3, 4})
                       .AddColumn<types::Int64Value>({1, 5, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 6, true, false)
                          .AddColumn<types::Int64Value>({1, 2, 3, 4, 5, 6})
                          .AddColumn<types::Int64Value>({2, 3, 3, 4, 1, 5})
                          .get(),
                      false)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, false, false)
                       .AddColumn<types::Int64Value>({1, 1, 2, 2})
                       .AddColumn<types::Int64Value>({2, 3, 3, 1})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, true)
                       .AddColumn<types::Int64Value>({5, 6, 3, 4})
                       .AddColumn<types::Int64Value>({1, 5, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(RowBatchBuilder(output_rd, 6, true, true)
                          .AddColumn<types::Int64Value>({1, 2, 3, 4, 5, 6})
                          .AddColumn<types::Int64Value>({2, 3, 3, 4, 1, 5})
                          .get(),
                      false)
      .Close();
}

TEST_F(AggNodeTest, no_aggregate_expressions) {
  auto plan_node = PlanNodeFromPbtxt(kSingleGroupNoValues);
  RowDescriptor input_rd({types::DataType::INT64, types::DataType::INT64});

  RowDescriptor output_rd({types::DataType::INT64});

  auto tester = exec::ExecNodeTester<AggNode, plan::AggregateOperator>(
      *plan_node, output_rd, {input_rd}, exec_state_.get());

  tester
      .ConsumeNext(RowBatchBuilder(input_rd, 4, /*eow*/ false, /*eos*/ false)
                       .AddColumn<types::Int64Value>({2, 1, 3, 1})
                       .AddColumn<types::Int64Value>({2, 5, 3, 1})
                       .get(),
                   0, 0)
      .ConsumeNext(RowBatchBuilder(input_rd, 4, true, true)
                       .AddColumn<types::Int64Value>({1, 2, 3, 3})
                       .AddColumn<types::Int64Value>({1, 3, 3, 8})
                       .get(),
                   0)
      .ExpectRowBatch(
          RowBatchBuilder(output_rd, 3, true, true).AddColumn<types::Int64Value>({2, 1, 3}).get(),
          false)
      .Close();
}

}  // namespace exec
}  // namespace carnot
}  // namespace px
