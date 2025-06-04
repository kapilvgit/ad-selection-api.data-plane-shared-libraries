// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/public/cpio/adapters/public_key_client/public_key_client.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "absl/synchronization/notification.h"
#include "src/core/interface/errors.h"
#include "src/public/core/interface/execution_result.h"
#include "src/public/core/test_execution_result_matchers.h"
#include "src/public/cpio/adapters/public_key_client/mock_public_key_client_with_overrides.h"
#include "src/public/cpio/interface/public_key_client/public_key_client_interface.h"
#include "src/public/cpio/interface/public_key_client/type_def.h"
#include "src/public/cpio/proto/public_key_service/v1/public_key_service.pb.h"

using google::cmrt::sdk::public_key_service::v1::ListPublicKeysRequest;
using google::cmrt::sdk::public_key_service::v1::ListPublicKeysResponse;
using google::scp::core::AsyncContext;
using google::scp::core::ExecutionResult;
using google::scp::core::FailureExecutionResult;
using google::scp::core::SuccessExecutionResult;
using google::scp::core::test::IsSuccessful;
using google::scp::core::test::ResultIs;
using google::scp::cpio::PublicKeyClient;
using google::scp::cpio::PublicKeyClientOptions;
using google::scp::cpio::mock::MockPublicKeyClientWithOverrides;
using testing::Return;

namespace google::scp::cpio::test {
class PublicKeyClientTest : public ::testing::Test {
 protected:
  PublicKeyClientTest() : client_(std::make_shared<PublicKeyClientOptions>()) {
    EXPECT_THAT(client_.Init(), IsSuccessful());
    EXPECT_THAT(client_.Run(), IsSuccessful());
  }

  ~PublicKeyClientTest() { EXPECT_THAT(client_.Stop(), IsSuccessful()); }

  MockPublicKeyClientWithOverrides client_;
};

TEST_F(PublicKeyClientTest, ListPublicKeysSuccess) {
  EXPECT_CALL(client_.GetPublicKeyClientProvider(), ListPublicKeys)
      .WillOnce([=](AsyncContext<ListPublicKeysRequest, ListPublicKeysResponse>&
                        context) {
        context.response = std::make_shared<ListPublicKeysResponse>();
        context.Finish(SuccessExecutionResult());
        return SuccessExecutionResult();
      });

  absl::Notification finished;
  EXPECT_THAT(client_.ListPublicKeys(ListPublicKeysRequest(),
                                     [&](const ExecutionResult result,
                                         ListPublicKeysResponse response) {
                                       EXPECT_THAT(result, IsSuccessful());
                                       finished.Notify();
                                     }),
              IsSuccessful());
  finished.WaitForNotification();
}

TEST_F(PublicKeyClientTest, ListPublicKeysFailure) {
  EXPECT_CALL(client_.GetPublicKeyClientProvider(), ListPublicKeys)
      .WillOnce([=](AsyncContext<ListPublicKeysRequest, ListPublicKeysResponse>&
                        context) {
        context.Finish(FailureExecutionResult(SC_UNKNOWN));
        return FailureExecutionResult(SC_UNKNOWN);
      });

  absl::Notification finished;
  EXPECT_THAT(
      client_.ListPublicKeys(
          ListPublicKeysRequest(),
          [&](const ExecutionResult result, ListPublicKeysResponse response) {
            EXPECT_THAT(result, ResultIs(FailureExecutionResult(SC_UNKNOWN)));
            finished.Notify();
          }),
      ResultIs(FailureExecutionResult(SC_UNKNOWN)));
  finished.WaitForNotification();
}

TEST_F(PublicKeyClientTest, FailureToCreatePublicKeyClientProvider) {
  auto failure = FailureExecutionResult(SC_UNKNOWN);
  client_.create_public_key_client_provider_result = failure;
  EXPECT_EQ(client_.Init(), failure);
}
}  // namespace google::scp::cpio::test
