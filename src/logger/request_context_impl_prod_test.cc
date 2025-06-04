/*
 * Copyright 2023 Google LLC
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
 */
#include "src/logger/request_context_impl_test.h"

namespace privacy_sandbox::server_common::log {
namespace {

using ::testing::ContainsRegex;
using ::testing::IsEmpty;
using ::testing::StrEq;

TEST_F(ConsentedLogTest, LogNotConsented) {
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{{"id", "1234"}},
      mismatched_token_);
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_THAT(LogWithCapturedStderr(
                  [this]() { PS_VLOG(kMaxV, *test_instance_) << kLogContent; }),
              IsEmpty());
  EXPECT_THAT(ReadSs(), IsEmpty());
}

TEST_F(ConsentedLogTest, LogConsented) {
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{{"id", "1234"}},
      matched_token_);
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_THAT(LogWithCapturedStderr(
                  [this]() { PS_VLOG(kMaxV, *test_instance_) << kLogContent; }),
              IsEmpty());
  EXPECT_THAT(ReadSs(),
              ContainsRegex(absl::StrCat("\\(id: 1234\\)[ \t]+", kLogContent)));
}

TEST_F(DebugResponseTest, NotLoggedInProd) {
  // mismatched_token_ doesn't log
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{{"id", "1234"}},
      mismatched_token_, [this]() {
        accessed_debug_info_ = true;
        return &debug_info_;
      });
  SetServerTokenForTestOnly(kServerToken);
  PS_VLOG(kMaxV, *test_instance_) << kLogContent;
  EXPECT_FALSE(accessed_debug_info_);

  // matched_token_ doesn't log
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{{"id", "1234"}}, matched_token_,
      [this]() {
        accessed_debug_info_ = true;
        return &debug_info_;
      });
  SetServerTokenForTestOnly(kServerToken);
  PS_VLOG(kMaxV, *test_instance_) << kLogContent;
  EXPECT_THAT(ReadSs(),
              ContainsRegex(absl::StrCat("\\(id: 1234\\)[ \t]+", kLogContent)));
  EXPECT_FALSE(accessed_debug_info_);

  // debug_info turned on, but doesn't log
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{{"id", "1234"}},
      debug_info_config_, [this]() {
        accessed_debug_info_ = true;
        return &debug_info_;
      });
  SetServerTokenForTestOnly(kServerToken);
  PS_VLOG(kMaxV, *test_instance_) << kLogContent;
  EXPECT_FALSE(accessed_debug_info_);
}

TEST(FormatContext, NoContextGeneratesEmptyString) {
  EXPECT_THAT(FormatContext({}), IsEmpty());
}

TEST(FormatContext, SingleKeyValFormatting) {
  EXPECT_THAT(FormatContext({{"key1", "val1"}}), StrEq(" (key1: val1) "));
}

TEST(FormatContext, MultipleKeysLexicographicallyOrdered) {
  EXPECT_THAT(FormatContext({{"key1", "val1"}, {"key2", "val2"}}),
              StrEq(" (key1: val1, key2: val2) "));
}

TEST(FormatContext, OptionalValuesNotInTheFormattedOutput) {
  EXPECT_THAT(FormatContext({{"key1", ""}}), IsEmpty());
}

TEST_F(ConsentedLogTest, NotConsented) {
  // default
  EXPECT_FALSE(ContextImpl({}, ConsentedDebugConfiguration()).is_consented());

  // no client token
  test_instance_ =
      std::make_unique<ContextImpl>(absl::btree_map<std::string, std::string>{},
                                    ConsentedDebugConfiguration());
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_FALSE(test_instance_->is_consented());

  // empty server and client token
  auto empty_client_token = ParseTextOrDie<ConsentedDebugConfiguration>(R"pb(
    is_consented: true
    token: ""
  )pb");
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{}, empty_client_token);
  SetServerTokenForTestOnly("");
  EXPECT_FALSE(test_instance_->is_consented());

  // empty client token, valid server token
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{}, empty_client_token);
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_FALSE(test_instance_->is_consented());

  // valid client token, empty server token
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{}, matched_token_);
  SetServerTokenForTestOnly("");
  EXPECT_FALSE(test_instance_->is_consented());

  // mismatch
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{}, mismatched_token_);
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_FALSE(test_instance_->is_consented());
}

TEST_F(ConsentedLogTest, ConsentRevocation) {
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{}, matched_token_);
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_TRUE(test_instance_->is_consented());

  matched_token_.set_is_consented(false);
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{}, matched_token_);
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_FALSE(test_instance_->is_consented());
}

TEST_F(ConsentedLogTest, Update) {
  test_instance_ = std::make_unique<ContextImpl>(
      absl::btree_map<std::string, std::string>{}, matched_token_);
  SetServerTokenForTestOnly(kServerToken);
  EXPECT_TRUE(test_instance_->is_consented());
  test_instance_->Update({}, ConsentedDebugConfiguration());
  EXPECT_FALSE(test_instance_->is_consented());
  test_instance_->Update({}, matched_token_);
  EXPECT_TRUE(test_instance_->is_consented());
  test_instance_->Update({}, mismatched_token_);
  EXPECT_FALSE(test_instance_->is_consented());
}

}  // namespace
}  // namespace privacy_sandbox::server_common::log
