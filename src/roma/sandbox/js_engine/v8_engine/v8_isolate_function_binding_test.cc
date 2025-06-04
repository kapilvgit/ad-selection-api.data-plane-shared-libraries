/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/roma/sandbox/js_engine/v8_engine/v8_isolate_function_binding.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "include/v8.h"
#include "src/core/test/utils/auto_init_run_stop.h"
#include "src/roma/sandbox/js_engine/v8_engine/v8_js_engine.h"
#include "src/roma/sandbox/native_function_binding/native_function_invoker.h"
#include "src/roma/sandbox/native_function_binding/rpc_wrapper.pb.h"

using google::scp::roma::proto::RpcWrapper;

using ::testing::_;
using ::testing::Return;
using ::testing::StrEq;

namespace google::scp::roma::sandbox::js_engine::test {
class V8IsolateFunctionBindingTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    js_engine::v8_js_engine::V8JsEngine engine;
    engine.OneTimeSetup();
  }
};

class NativeFunctionInvokerMock
    : public native_function_binding::NativeFunctionInvoker {
 public:
  MOCK_METHOD(absl::Status, Invoke, (RpcWrapper&), (override));

  virtual ~NativeFunctionInvokerMock() = default;
};

TEST_F(V8IsolateFunctionBindingTest, FunctionBecomesAvailableInJavascript) {
  auto function_invoker = std::make_unique<NativeFunctionInvokerMock>();
  EXPECT_CALL(*function_invoker, Invoke(_)).WillOnce(Return(absl::OkStatus()));

  std::vector<std::string> function_names = {"cool_func"};
  auto visitor = std::make_unique<v8_js_engine::V8IsolateFunctionBinding>(
      function_names, std::move(function_invoker), /*server_address=*/"");

  js_engine::v8_js_engine::V8JsEngine js_engine(std::move(visitor));
  js_engine.Run();

  auto result_or = js_engine.CompileAndRunJs(
      R"(function func() { cool_func(); return ""; })", "func", {}, {});
  ASSERT_TRUE(result_or.ok());
  const auto& response_string = result_or->execution_response.response;
  EXPECT_THAT(response_string, StrEq(R"("")"));
  js_engine.Stop();
}

}  // namespace google::scp::roma::sandbox::js_engine::test
