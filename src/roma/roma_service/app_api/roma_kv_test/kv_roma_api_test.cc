/*
 * Copyright 2023 Google LLC
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "kv_romav8_app_service.h"

using ::testing::Contains;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::StrEq;

using ::privacysandbox::kvserver::roma_app_api::KeyValueService;
using ::privacysandbox::roma::app_api::kv_test::v1::GetValuesRequest;
using ::privacysandbox::roma::app_api::kv_test::v1::GetValuesResponse;

namespace privacysandbox::kvserver::roma::AppApi::RomaKvTest {

class RomaV8AppTest : public ::testing::Test {
 protected:
  void SetUp() override {
    google::scp::roma::Config cfg;
    cfg.number_of_workers = 2;
    roma_service_ = std::make_unique<
        google::scp::roma::sandbox::roma_service::RomaService<>>(
        std::move(cfg));
    EXPECT_TRUE(roma_service_->Init().ok());
  }

  void TearDown() override {
    const absl::Status status = roma_service_->Stop();
    EXPECT_TRUE(status.ok());
  }

 protected:
  std::unique_ptr<RomaService<>> roma_service_;
};

namespace {
const absl::Duration kDefaultTimeout = absl::Seconds(10);
}

TEST_F(RomaV8AppTest, EncodeDecodeSimpleProtobuf) {
  KeyValueService<> app_svc(*roma_service_);

  constexpr std::string_view jscode = R"(
    KvServer.GetValues = function(req) {
      return {
        abc: 123,
        unknown1: "An unknown field",
        foo: { foobar1: 123 },
        value: "Something in the reply",
        vals: ["Hi Foobar!"],
      };
    };
  )";
  absl::Notification register_finished;
  absl::Status register_status;
  ASSERT_TRUE(
      app_svc.Register(register_finished, register_status, jscode).ok());
  register_finished.WaitForNotificationWithTimeout(kDefaultTimeout);
  EXPECT_TRUE(register_status.ok());

  absl::Notification completed;
  GetValuesRequest req;
  GetValuesResponse resp;
  ASSERT_TRUE(app_svc.GetValues(completed, req, resp).ok());
  completed.WaitForNotificationWithTimeout(kDefaultTimeout);

  EXPECT_THAT(resp.vals(), Contains("Hi Foobar!"));
  EXPECT_THAT(resp.value(), StrEq("Something in the reply"));
  EXPECT_THAT(resp.foo().foobar1(), Eq(123));
  EXPECT_THAT(resp.foos_size(), Eq(0));
}

TEST_F(RomaV8AppTest, EncodeDecodeEmptyProtobuf) {
  KeyValueService<> app_svc(*roma_service_);

  constexpr std::string_view jscode = R"(
    KvServer.GetValues = function(req) {
      return {
        foo: {},
        foos: [],
        value: "",
        vals: [],
      };
    };
  )";
  absl::Notification register_finished;
  absl::Status register_status;
  ASSERT_TRUE(
      app_svc.Register(register_finished, register_status, jscode).ok());
  register_finished.WaitForNotificationWithTimeout(kDefaultTimeout);
  EXPECT_TRUE(register_status.ok());

  absl::Notification completed;
  GetValuesRequest req;
  GetValuesResponse resp;
  ASSERT_TRUE(app_svc.GetValues(completed, req, resp).ok());
  completed.WaitForNotificationWithTimeout(kDefaultTimeout);

  EXPECT_THAT(resp.vals(), IsEmpty());
  EXPECT_THAT(resp.value(), IsEmpty());
  EXPECT_THAT(resp.foo().foobar1(), Eq(0));
  ASSERT_THAT(resp.foos_size(), Eq(0));
}

TEST_F(RomaV8AppTest, EncodeDecodeRepeatedMessageProtobuf) {
  KeyValueService<> app_svc(*roma_service_);

  constexpr std::string_view jscode = R"(
    KvServer.GetValues = function(req) {
      return {
        vals: [
          "str1",
          "str2",
          "str1",
          "str2",
        ],
        foos: [
          { foobar1: 123 },
          { foobar2: "abc123" },
        ],
        abc: 123,
        unknown1: "An unknown field",
        foo: { foobar1: 123 },
        value: "Something in the reply",
      };
    };
  )";
  absl::Notification register_finished;
  absl::Status register_status;
  ASSERT_TRUE(
      app_svc.Register(register_finished, register_status, jscode).ok());
  register_finished.WaitForNotificationWithTimeout(kDefaultTimeout);
  EXPECT_TRUE(register_status.ok());

  absl::Notification completed;
  GetValuesRequest req;
  GetValuesResponse resp;
  ASSERT_TRUE(app_svc.GetValues(completed, req, resp).ok());
  completed.WaitForNotificationWithTimeout(kDefaultTimeout);

  EXPECT_THAT(resp.vals_size(), Eq(4));
  ASSERT_THAT(resp.foos_size(), Eq(2));
  EXPECT_THAT(resp.foos(0).foobar1(), Eq(123));
  EXPECT_THAT(resp.foos(1).foobar2(), StrEq("abc123"));
}

TEST_F(RomaV8AppTest, UseRequestField) {
  KeyValueService<> app_svc(*roma_service_);

  constexpr std::string_view jscode = R"(
    KvServer.GetValues = function(req) {
      return {
        value: "Something in the reply",
        vals: ["Hi " + req.keysList[0] + "!"],
      };
    };
  )";
  absl::Notification register_finished;
  absl::Status register_status;
  ASSERT_TRUE(
      app_svc.Register(register_finished, register_status, jscode).ok());
  register_finished.WaitForNotificationWithTimeout(kDefaultTimeout);
  EXPECT_TRUE(register_status.ok());

  absl::Notification completed;
  GetValuesRequest req;
  req.set_key1("abc123");
  req.set_key2("def123");
  req.add_keys("Foobar");
  GetValuesResponse resp;
  ASSERT_TRUE(app_svc.GetValues(completed, req, resp).ok());
  completed.WaitForNotificationWithTimeout(kDefaultTimeout);

  EXPECT_THAT(resp.vals(), Contains("Hi Foobar!"));
  EXPECT_THAT(resp.value(), StrEq("Something in the reply"));
}

}  // namespace privacysandbox::kvserver::roma::AppApi::RomaKvTest
