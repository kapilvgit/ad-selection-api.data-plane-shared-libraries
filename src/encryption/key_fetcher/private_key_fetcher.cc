// Copyright 2023 Google LLC
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

#include "src/encryption/key_fetcher/private_key_fetcher.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_join.h"
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "proto/hpke.pb.h"
#include "proto/tink.pb.h"
#include "src/core/interface/errors.h"
#include "src/core/interface/type_def.h"
#include "src/encryption/key_fetcher/key_fetcher_utils.h"
#include "src/metric/key_fetch.h"
#include "src/public/core/interface/execution_result.h"
#include "src/public/cpio/interface/private_key_client/private_key_client_interface.h"
#include "src/public/cpio/interface/private_key_client/type_def.h"

using google::scp::core::ExecutionResult;
using google::scp::core::FailureExecutionResult;
using google::scp::core::SuccessExecutionResult;
using google::scp::core::errors::GetErrorMessage;

using google::cmrt::sdk::private_key_service::v1::ListPrivateKeysRequest;
using google::cmrt::sdk::private_key_service::v1::ListPrivateKeysResponse;
using ::google::scp::cpio::PrivateKeyClientInterface;
using ::google::scp::cpio::PrivateKeyClientOptions;

using ::google::scp::core::PublicPrivateKeyPairId;

using ::google::scp::cpio::AccountIdentity;
using ::google::scp::cpio::PrivateKeyVendingEndpoint;
using ::google::scp::cpio::PrivateKeyVendingServiceEndpoint;
using ::google::scp::cpio::Region;

namespace privacy_sandbox::server_common {
namespace {

constexpr std::string_view kKeyFetchFailMessage =
    "GetEncryptedPrivateKey call failed (key IDs: $0, status_code: $1)";

absl::Status HandleFailure(
    const google::protobuf::RepeatedPtrField<std::string>& key_ids,
    google::scp::core::StatusCode status_code) noexcept {
  std::string key_ids_str = absl::StrJoin(key_ids, ", ");
  const std::string error = absl::Substitute(kKeyFetchFailMessage, key_ids_str,
                                             GetErrorMessage(status_code));
  VLOG(1) << error;
  return absl::UnavailableError(error);
}

absl::Time ProtoToAbslDuration(const google::protobuf::Timestamp& timestamp) {
  return absl::FromUnixSeconds(timestamp.seconds()) +
         absl::Nanoseconds(timestamp.nanos());
}

}  // namespace

PrivateKeyFetcher::PrivateKeyFetcher(
    std::unique_ptr<google::scp::cpio::PrivateKeyClientInterface>
        private_key_client,
    absl::Duration ttl)
    : private_key_client_(std::move(private_key_client)), ttl_(ttl) {}

PrivateKeyFetcher::~PrivateKeyFetcher() {
  if (ExecutionResult result = private_key_client_->Stop();
      !result.Successful()) {
    VLOG(1) << GetErrorMessage(result.status_code);
  }
}

absl::Status PrivateKeyFetcher::Refresh() noexcept ABSL_LOCKS_EXCLUDED(mutex_) {
  VLOG(3) << "Refreshing private keys...";

  ListPrivateKeysRequest request;
  request.set_max_age_seconds(ToInt64Seconds(ttl_));

  absl::Notification fetch_notify;
  auto list_priv_key_cb = [request, &fetch_notify, this](
                              const ExecutionResult result,
                              const ListPrivateKeysResponse response) {
    if (result.Successful()) {
      KeyFetchResultCounter::SetNumPrivateKeysFetched(
          response.private_keys().size());
      absl::MutexLock l(&mutex_);
      int num_priv_keys_added = 0;
      for (const auto& private_key : response.private_keys()) {
        std::string keyset_bytes;
        if (!absl::Base64Unescape(private_key.private_key(), &keyset_bytes)) {
          LOG(ERROR) << "Could not base 64 decode the keyset. Key Id: "
                     << private_key.key_id();
          continue;
        }
        google::crypto::tink::Keyset keyset;
        if (!keyset.ParseFromString(keyset_bytes)) {
          LOG(ERROR) << "Could not parse a tink::Keyset from the base 64 "
                        "decoded bytes. Key Id: "
                     << private_key.key_id();
          continue;
        }
        if (keyset.key().size() != 1) {
          LOG(ERROR) << "Keyset must contain exactly one key. Key Id: "
                     << private_key.key_id();
          continue;
        }
        std::string hpke_priv_key_bytes = keyset.key()[0].key_data().value();
        google::crypto::tink::HpkePrivateKey hpke_priv_key;
        if (!hpke_priv_key.ParseFromString(hpke_priv_key_bytes)) {
          LOG(ERROR) << "Could not parse the tink::HpkePrivateKey from the "
                        "raw bytes. Key Id: "
                     << private_key.key_id();
          continue;
        }
        PrivateKey key = {
            ToOhttpKeyId(private_key.key_id()),
            hpke_priv_key.private_key(),
            ProtoToAbslDuration(private_key.creation_time()),
        };
        private_keys_map_.insert_or_assign(key.key_id, std::move(key));
        ++num_priv_keys_added;
        if (VLOG_IS_ON(2)) {
          VLOG(2) << absl::StrCat(
              "Caching private key: (KMS id: ", private_key.key_id(),
              ", OHTTP ID: ", key.key_id, ")");
        }
      }
      KeyFetchResultCounter::SetNumPrivateKeysParsed(num_priv_keys_added);
    } else {
      KeyFetchResultCounter::IncrementPrivateKeyFetchAsyncFailureCount();
      KeyFetchResultCounter::SetNumPrivateKeysParsed(0);
      HandleFailure(request.key_ids(), result.status_code).IgnoreError();
    }
    fetch_notify.Notify();
  };
  if (const ExecutionResult result = private_key_client_->ListPrivateKeys(
          request, std::move(list_priv_key_cb));
      !result.Successful()) {
    return HandleFailure(request.key_ids(), result.status_code);
  }

  VLOG(3) << "Private key fetch pending...";
  fetch_notify.WaitForNotification();

  absl::MutexLock l(&mutex_);
  // Clean up keys that have been stored in the cache for longer than the ttl.
  absl::Time cutoff_time = absl::Now() - ttl_;
  VLOG(3) << "Cleaning up private keys with cutoff time: " << cutoff_time;
  for (auto it = private_keys_map_.cbegin(); it != private_keys_map_.cend();) {
    if (it->second.creation_time < cutoff_time) {
      private_keys_map_.erase(it++);
    } else {
      it++;
    }
  }
  KeyFetchResultCounter::SetNumPrivateKeysCached(private_keys_map_.size());
  return absl::OkStatus();
}

std::optional<PrivateKey> PrivateKeyFetcher::GetKey(
    const google::scp::cpio::PublicPrivateKeyPairId& public_key_id) noexcept
    ABSL_LOCKS_EXCLUDED(mutex_) {
  absl::MutexLock l(&mutex_);
  if (const auto it = private_keys_map_.find(public_key_id);
      it != private_keys_map_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::unique_ptr<PrivateKeyFetcherInterface> PrivateKeyFetcherFactory::Create(
    const PrivateKeyVendingEndpoint& primary_endpoint,
    const std::vector<PrivateKeyVendingEndpoint>& secondary_endpoints,
    absl::Duration key_ttl) {
  PrivateKeyClientOptions options;
  options.primary_private_key_vending_endpoint = primary_endpoint;
  options.secondary_private_key_vending_endpoints = secondary_endpoints;

  std::unique_ptr<PrivateKeyClientInterface> private_key_client =
      google::scp::cpio::PrivateKeyClientFactory::Create(options);
  if (const ExecutionResult init_result = private_key_client->Init();
      !init_result.Successful()) {
    VLOG(1) << "Failed to initialize private key client.";
  }
  if (const ExecutionResult run_result = private_key_client->Run();
      !run_result.Successful()) {
    VLOG(1) << "Failed to run private key client.";
  }
  return std::make_unique<PrivateKeyFetcher>(std::move(private_key_client),
                                             key_ttl);
}

}  // namespace privacy_sandbox::server_common
